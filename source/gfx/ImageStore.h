#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx/ImageDecode.h"
#include "gfx/Renderer.h"

namespace api {
class Client;
}
namespace util {
class TaskQueue;
}

namespace gfx {

// Artwork cache: the UI asks for a URL every frame and gets TexInvalid until
// the bytes have been fetched, decoded on a worker and uploaded on the render
// thread. Mirrors the web client's lazy <img> with its fade-in on arrival.
class ImageStore {
public:
    void init(Renderer* renderer, api::Client* client, util::TaskQueue* tasks);
    void shutdown();

    // `size` is the server's variant (thumb / medium / full); `maxWidth` is
    // the decode cap in physical pixels.
    TexId get(const std::string& url, const char* size, int maxWidth);

    // Fade factor for the first frames after a texture landed, so callers can
    // reveal it the way `card-img-reveal` does. Takes the key `get` built.
    float revealAlpha(const std::string& key) const;
    static std::string keyFor(const std::string& url, const char* size, int maxWidth);

    void tick();

private:
    struct Entry {
        TexId tex = TexInvalid;
        bool loading = false;
        bool failed = false;
        uint64_t lastUsedFrame = 0;
        uint64_t readyFrame = 0;
        size_t bytes = 0;
    };

    struct Pending {
        std::string key;
        DecodedImage image;
        bool failed = false;
    };

    void evictIfNeeded();

    Renderer* m_renderer = nullptr;
    api::Client* m_client = nullptr;
    util::TaskQueue* m_tasks = nullptr;

    std::unordered_map<std::string, Entry> m_entries;
    std::vector<Pending> m_pending;
    std::mutex m_pendingMutex;

    uint64_t m_frame = 0;
    size_t m_bytesResident = 0;
    int m_inFlight = 0;

    // Set from Renderer::textureBudget() at init: a title takeover can hold
    // a screenful of 2x artwork plus fanarts, applet mode cannot come close.
    size_t m_budgetBytes = 20u * 1024 * 1024;
    static constexpr int kMaxInFlight = 6;
};

} // namespace gfx
