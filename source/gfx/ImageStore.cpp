#include "gfx/ImageStore.h"

#include <algorithm>

#include "util/Log.h"

#include "net/Api.h"
#include "util/TaskQueue.h"

namespace gfx {

void ImageStore::init(Renderer* renderer, api::Client* client, util::TaskQueue* tasks)
{
    m_renderer = renderer;
    m_client = client;
    m_tasks = tasks;
    m_budgetBytes = Renderer::textureBudget();
    FLIKS_LOG("images: %zu MiB texture budget", m_budgetBytes / (1024 * 1024));
}

void ImageStore::shutdown()
{
    for (auto& kv : m_entries)
        if (kv.second.tex != TexInvalid) m_renderer->destroyTexture(kv.second.tex);
    m_entries.clear();
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pending.clear();
}

std::string ImageStore::keyFor(const std::string& url, const char* size, int maxWidth)
{
    // The decode width is part of the identity: the same poster used as a
    // rail card and as a detail-page hero must not share one texture.
    return url + "|" + (size ? size : "") + "|" + std::to_string(maxWidth);
}

TexId ImageStore::get(const std::string& url, const char* size, int maxWidth)
{
    if (url.empty()) return TexInvalid;

    const std::string key = keyFor(url, size, maxWidth);

    auto it = m_entries.find(key);
    if (it != m_entries.end()) {
        it->second.lastUsedFrame = m_frame;
        return it->second.tex;
    }

    // Nothing is recorded while the queue is saturated, so the next frame
    // re-requests it naturally once a slot frees up. Capping here is what
    // keeps a fast scroll from queueing several hundred downloads.
    if (m_inFlight >= kMaxInFlight) return TexInvalid;
    m_inFlight++;

    Entry entry;
    entry.loading = true;
    entry.lastUsedFrame = m_frame;
    m_entries.emplace(key, entry);

    const std::string absolute = m_client->imageUrl(url, size);
    m_tasks->post([this, key, absolute, maxWidth] {
        Pending pending;
        pending.key = key;
        std::string bytes;
        if (m_client->fetchBytes(absolute, bytes))
            pending.failed = !decodeImage(bytes, maxWidth, pending.image);
        else
            pending.failed = true;

        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pending.push_back(std::move(pending));
    });

    return TexInvalid;
}

float ImageStore::revealAlpha(const std::string& key) const
{
    auto it = m_entries.find(key);
    if (it == m_entries.end() || it->second.readyFrame == 0) return 1.0f;
    // `card-img-reveal`: 200ms ease-out, which is 12 frames at 60Hz.
    const uint64_t age = m_frame - it->second.readyFrame;
    if (age >= 12) return 1.0f;
    const float t = static_cast<float>(age) / 12.0f;
    return 1.0f - (1.0f - t) * (1.0f - t);
}

void ImageStore::tick()
{
    m_frame++;

    std::vector<Pending> batch;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        batch.swap(m_pending);
    }

    for (Pending& p : batch) {
        m_inFlight = std::max(0, m_inFlight - 1);
        auto it = m_entries.find(p.key);
        if (it == m_entries.end()) continue;

        it->second.loading = false;
        if (p.failed || !p.image.valid()) {
            it->second.failed = true;
            continue;
        }
        const TexId tex = m_renderer->createTexture(p.image.width, p.image.height,
                                                    DkImageFormat_RGBA8_Unorm, p.image.rgba.data());
        if (tex == TexInvalid) {
            it->second.failed = true;
            continue;
        }
        it->second.tex = tex;
        it->second.readyFrame = m_frame;
        it->second.bytes = p.image.rgba.size();
        m_bytesResident += it->second.bytes;
    }

    evictIfNeeded();
}

void ImageStore::evictIfNeeded()
{
    if (m_bytesResident <= m_budgetBytes) return;

    std::vector<std::pair<uint64_t, std::string>> candidates;
    candidates.reserve(m_entries.size());
    for (const auto& kv : m_entries) {
        // Anything drawn this frame or the last is still on screen.
        if (kv.second.tex == TexInvalid || kv.second.lastUsedFrame + 2 >= m_frame) continue;
        candidates.emplace_back(kv.second.lastUsedFrame, kv.first);
    }
    std::sort(candidates.begin(), candidates.end());

    for (const auto& c : candidates) {
        if (m_bytesResident <= m_budgetBytes) break;
        auto it = m_entries.find(c.second);
        if (it == m_entries.end()) continue;
        m_renderer->destroyTexture(it->second.tex);
        m_bytesResident -= std::min(m_bytesResident, it->second.bytes);
        m_entries.erase(it);
    }
}

} // namespace gfx
