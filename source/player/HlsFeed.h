#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "util/Thread.h"

namespace player {

// Fliks serves fragmented-MP4 HLS: an `#EXT-X-MAP` init segment followed by
// `.m4s` fragments. Concatenated, those bytes *are* a valid fMP4 stream, so
// the playlist is walked here and ffmpeg is handed one continuous byte
// stream through a single AVIOContext.
//
// That removes ffmpeg's HLS demuxer from the picture entirely, along with
// the custom io_open, the protocol whitelist, and the segment close/reopen
// dance — which is where playback wedged: a fragment would download in full,
// hit EOF, and yield no packets.
//
// Fetching runs on its own thread and reads ahead. Opening a segment costs a
// TCP connection and an HTTP round trip, and doing that inline every three
// seconds stalled the demuxer long enough to drain the frame queue — the
// stutter was once per fragment, exactly.
//
// Seeking is not offered at the byte level. A seek means "restart at another
// segment", which the player does by rebuilding the feed.
class HlsFeed {
public:
    // Roughly six video fragments of lookahead, which is far more than a
    // segment fetch takes even on a slow link.
    static constexpr size_t kMaxBuffered = 8 * 1024 * 1024;

    struct Segment {
        std::string url;
        double duration = 0;
        double start = 0;
    };

    // `ringBytes` sizes the read-ahead. The default suits video; an audio
    // rendition wants far less, or its producer races minutes ahead and takes
    // the link away from the video feed exactly when the picture needs it.
    explicit HlsFeed(size_t ringBytes = kMaxBuffered);
    ~HlsFeed();
    HlsFeed(const HlsFeed&) = delete;
    HlsFeed& operator=(const HlsFeed&) = delete;

    // `body` is the media playlist already fetched by the caller.
    bool parse(const std::string& playlistUrl, const std::string& body);

    // Positions at the segment containing `seconds` and starts reading ahead.
    // The first bytes handed out are the init segment, so what ffmpeg sees is
    // always a complete stream.
    bool open(double seconds);
    void close();
    void abort();

    // Blocks until bytes are available, the stream ends (0) or fails (-1).
    int read(uint8_t* buf, int len);

    // Presentation time of the segment the feed actually started on, which is
    // what the player's clock has to be anchored to.
    double startTime() const { return m_startTime; }
    double totalDuration() const;
    size_t segmentCount() const { return m_segments.size(); }
    size_t bufferedBytes() const;

private:
    void produce();
    bool openNext();

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    std::string m_playlistUrl;
    std::string m_initUrl;
    std::vector<Segment> m_segments;
    size_t m_nextSegment = 0;
    double m_startTime = 0;
    bool m_initPending = false;

    static constexpr size_t kChunkSize = 64 * 1024;
    size_t m_ringBytes = kMaxBuffered;

    util::Thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    // A fixed ring rather than a queue of buffers: at 64 KiB a read and tens
    // of megabytes a minute, allocating per chunk is churn the allocator does
    // not need while the decoder is competing for the same cores.
    std::vector<uint8_t> m_ring;
    size_t m_head = 0;
    size_t m_tail = 0;
    size_t m_buffered = 0;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_finished{ false };
    std::atomic<bool> m_failed{ false };
};

} // namespace player
