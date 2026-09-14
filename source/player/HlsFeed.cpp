#include "player/HlsFeed.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <switch.h>

#include "net/Http.h"
#include "util/Log.h"

namespace player {

namespace {

std::string trimmed(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// URI="init.mp4"
std::string attributeUri(const std::string& line)
{
    const size_t at = line.find("URI=\"");
    if (at == std::string::npos) return {};
    const size_t start = at + 5;
    const size_t end = line.find('"', start);
    if (end == std::string::npos) return {};
    return line.substr(start, end - start);
}


} // namespace

struct HlsFeed::Impl {
    net::Stream stream;
    bool open = false;
};

HlsFeed::HlsFeed(size_t ringBytes)
    : m_impl(new Impl), m_ringBytes(std::max<size_t>(ringBytes, kChunkSize * 4))
{
}
HlsFeed::~HlsFeed() { close(); }

bool HlsFeed::parse(const std::string& playlistUrl, const std::string& body)
{
    m_playlistUrl = playlistUrl;
    m_segments.clear();
    m_initUrl.clear();

    double clock = 0.0;
    double pendingDuration = 0.0;
    size_t pos = 0;

    while (pos < body.size()) {
        size_t eol = body.find('\n', pos);
        if (eol == std::string::npos) eol = body.size();
        const std::string line = trimmed(body.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty()) continue;

        if (line.rfind("#EXT-X-MAP:", 0) == 0) {
            const std::string uri = attributeUri(line);
            if (!uri.empty()) m_initUrl = net::joinUrl(playlistUrl, uri);
            continue;
        }
        if (line.rfind("#EXTINF:", 0) == 0) {
            pendingDuration = std::atof(line.c_str() + 8);
            continue;
        }
        if (line[0] == '#') continue;

        Segment seg;
        seg.url = net::joinUrl(playlistUrl, line);
        seg.duration = pendingDuration > 0 ? pendingDuration : 0.0;
        seg.start = clock;
        clock += seg.duration;
        pendingDuration = 0.0;
        m_segments.push_back(std::move(seg));
    }

    FLIKS_LOG("hls: media playlist, %zu segments, %.1fs, init=%s", m_segments.size(), clock,
              m_initUrl.empty() ? "none" : "yes");
    return !m_segments.empty();
}

double HlsFeed::totalDuration() const
{
    if (m_segments.empty()) return 0.0;
    return m_segments.back().start + m_segments.back().duration;
}

size_t HlsFeed::bufferedBytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_buffered;
}

bool HlsFeed::open(double seconds)
{
    close();
    if (m_segments.empty()) return false;

    // Land on the segment containing the requested time; a fragment is only
    // decodable from its own start.
    size_t index = 0;
    for (size_t i = 0; i < m_segments.size(); i++) {
        if (m_segments[i].start <= seconds) index = i;
        else break;
    }

    m_nextSegment = index;
    m_startTime = m_segments[index].start;
    m_initPending = !m_initUrl.empty();
    m_finished = false;
    m_failed = false;
    m_ring.assign(m_ringBytes, 0);
    m_head = m_tail = m_buffered = 0;

    FLIKS_LOG("hls: feed starting at segment %zu (t=%.1f)", index, m_startTime);

    m_running = true;
    if (!m_thread.start([this] { produce(); }, util::Thread::kWorkerStack)) {
        m_running = false;
        return false;
    }
    return true;
}

bool HlsFeed::openNext()
{
    m_impl.reset(new Impl);

    std::string url;
    if (m_initPending) {
        url = m_initUrl;
        m_initPending = false;
    } else if (m_nextSegment < m_segments.size()) {
        url = m_segments[m_nextSegment].url;
        m_nextSegment++;
    } else {
        return false;
    }

    if (!m_impl->stream.open(url, {}, 0)) {
        FLIKS_LOG("hls: could not fetch %s", util::loggableUrl(url).c_str());
        m_failed = true;
        return false;
    }
    m_impl->open = true;
    return true;
}

void HlsFeed::produce()
{
    std::vector<uint8_t> scratch(kChunkSize);

    while (m_running.load()) {
        {
            // Read-ahead is bounded so a fast link cannot pull the whole film
            // into memory ahead of the decoder.
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_ring.size() - m_buffered < kChunkSize) {
                m_cv.wait_for(lock, std::chrono::milliseconds(20));
                continue;
            }
        }

        if (!m_impl->open && !openNext()) {
            m_finished = true;
            m_cv.notify_all();
            return;
        }

        const int n = m_impl->stream.read(scratch.data(), static_cast<int>(scratch.size()));
        if (n > 0) {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (int i = 0; i < n; i++) {
                m_ring[m_tail] = scratch[static_cast<size_t>(i)];
                m_tail = (m_tail + 1) % m_ring.size();
            }
            m_buffered += static_cast<size_t>(n);
            m_cv.notify_all();
            continue;
        }
        if (n < 0) {
            FLIKS_LOG("hls: read error, stopping");
            m_failed = true;
            m_finished = true;
            m_cv.notify_all();
            return;
        }

        // End of this fragment; the next one continues the same byte stream,
        // so the consumer never sees a boundary.
        m_impl->stream.close();
        m_impl->open = false;
        if (m_nextSegment >= m_segments.size()) {
            m_finished = true;
            m_cv.notify_all();
            return;
        }
    }
}

int HlsFeed::read(uint8_t* buf, int len)
{
    if (len <= 0) return 0;

    std::unique_lock<std::mutex> lock(m_mutex);
    while (m_buffered == 0) {
        if (m_failed.load()) return -1;
        if (m_finished.load() || !m_running.load()) return 0;
        m_cv.wait_for(lock, std::chrono::milliseconds(20));
    }

    const size_t n = std::min(m_buffered, static_cast<size_t>(len));
    for (size_t i = 0; i < n; i++) {
        buf[i] = m_ring[m_head];
        m_head = (m_head + 1) % m_ring.size();
    }
    m_buffered -= n;
    m_cv.notify_all();
    return static_cast<int>(n);
}

void HlsFeed::close()
{
    m_running = false;
    if (m_impl) m_impl->stream.abort();
    m_cv.notify_all();
    m_thread.join();

    if (m_impl) {
        m_impl->stream.close();
        m_impl->open = false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_head = m_tail = m_buffered = 0;
}

void HlsFeed::abort()
{
    m_failed = true;
    m_running = false;
    if (m_impl) m_impl->stream.abort();
    m_cv.notify_all();
}

} // namespace player
