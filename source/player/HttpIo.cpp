#include "player/HttpIo.h"

#include <memory>
#include <mutex>
#include <set>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

#include "net/Http.h"
#include "player/HlsFeed.h"
#include "util/Log.h"

namespace player {

namespace {

constexpr int kIoBufferSize = 65536;

struct IoStream {
    net::Stream stream;
    std::string url;
    bool failed = false;
    int64_t readSoFar = 0;
    int64_t loggedAt = 0;
};

// Every stream a context opened, so an abort can reach the one currently
// blocked in recv.
struct Registry {
    std::mutex mutex;
    std::set<IoStream*> live;
};

Registry& registry()
{
    static Registry instance;
    return instance;
}


int ioRead(void* opaque, uint8_t* buf, int bufSize)
{
    auto* io = static_cast<IoStream*>(opaque);
    const int n = io->stream.read(buf, bufSize);
    if (n == 0) {
        FLIKS_LOG("io: eof %s at %lld", util::loggableUrl(io->url).c_str(),
                  static_cast<long long>(io->readSoFar));
        return AVERROR_EOF;
    }
    if (n < 0) {
        io->failed = true;
        FLIKS_LOG("io: error %s at %lld", util::loggableUrl(io->url).c_str(),
                  static_cast<long long>(io->readSoFar));
        return AVERROR(EIO);
    }
    io->readSoFar += n;
    if (io->readSoFar - io->loggedAt >= 8 * 1024 * 1024) {
        io->loggedAt = io->readSoFar;
        FLIKS_LOG("io: read %s %lld/%lld", util::loggableUrl(io->url).c_str(),
                  static_cast<long long>(io->readSoFar),
                  static_cast<long long>(io->stream.size()));
    }
    return n;
}

int64_t ioSeek(void* opaque, int64_t offset, int whence)
{
    auto* io = static_cast<IoStream*>(opaque);
    if (whence == AVSEEK_SIZE) return io->stream.size();
    if (whence & AVSEEK_FORCE) whence &= ~AVSEEK_FORCE;
    const int64_t rc = io->stream.seek(offset, whence);
    FLIKS_LOG("io: seek %s to %lld whence=%d -> %lld", util::loggableUrl(io->url).c_str(),
              static_cast<long long>(offset), whence, static_cast<long long>(rc));
    return rc;
}

AVIOContext* openStream(const std::string& url)
{
    auto io = std::make_unique<IoStream>();
    io->url = url;
    if (!io->stream.open(url, {}, 0)) {
        FLIKS_LOG("io: open failed %s", util::loggableUrl(url).c_str());
        return nullptr;
    }
    FLIKS_LOG("io: open %s (%lld bytes, seekable=%d)", util::loggableUrl(url).c_str(),
              static_cast<long long>(io->stream.size()), io->stream.seekable() ? 1 : 0);

    auto* buffer = static_cast<unsigned char*>(av_malloc(kIoBufferSize));
    if (!buffer) return nullptr;

    AVIOContext* ctx =
        avio_alloc_context(buffer, kIoBufferSize, 0, io.get(), ioRead, nullptr, ioSeek);
    if (!ctx) {
        av_free(buffer);
        return nullptr;
    }
    ctx->seekable = io->stream.seekable() ? AVIO_SEEKABLE_NORMAL : 0;
    // io_open hands back a context ffmpeg will walk in small steps; a demuxer
    // that believes it cannot seek takes very different, slower paths.


    {
        std::lock_guard<std::mutex> lock(registry().mutex);
        registry().live.insert(io.get());
    }
    io.release();   // owned by the AVIOContext's opaque from here
    return ctx;
}

void closeStream(AVIOContext* pb)
{
    if (!pb) return;
    auto* io = static_cast<IoStream*>(pb->opaque);
    if (io) FLIKS_LOG("io: close %s", util::loggableUrl(io->url).c_str());
    if (io) {
        std::lock_guard<std::mutex> lock(registry().mutex);
        registry().live.erase(io);
    }
    av_freep(&pb->buffer);
    avio_context_free(&pb);
    delete io;
}

// The HLS demuxer resolves its variant playlists and segments through this,
// so they take the same TLS path as the master.
int hookIoOpen(AVFormatContext* s, AVIOContext** pb, const char* url, int flags,
               AVDictionary** options)
{
    (void)s;
    (void)flags;
    (void)options;
    AVIOContext* ctx = openStream(url ? url : "");
    if (!ctx) return AVERROR(EIO);
    *pb = ctx;
    return 0;
}

int hookIoClose(AVFormatContext* s, AVIOContext* pb)
{
    (void)s;
    closeStream(pb);
    return 0;
}

} // namespace

namespace {

int feedRead(void* opaque, uint8_t* buf, int bufSize)
{
    auto* feed = static_cast<HlsFeed*>(opaque);
    const int n = feed->read(buf, bufSize);
    if (n == 0) return AVERROR_EOF;
    if (n < 0) return AVERROR(EIO);
    return n;
}

} // namespace

bool attachFeedIo(AVFormatContext* fmt, HlsFeed* feed)
{
    auto* buffer = static_cast<unsigned char*>(av_malloc(kIoBufferSize));
    if (!buffer) return false;

    // No seek callback: the stream is a concatenation, and seeking means
    // restarting the feed at another segment, which the player handles.
    AVIOContext* ctx = avio_alloc_context(buffer, kIoBufferSize, 0, feed, feedRead, nullptr,
                                          nullptr);
    if (!ctx) {
        av_free(buffer);
        return false;
    }
    ctx->seekable = 0;
    fmt->pb = ctx;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    return true;
}

void detachFeedIo(AVIOContext* pb)
{
    if (!pb) return;
    av_freep(&pb->buffer);
    avio_context_free(&pb);
}

bool attachHttpIo(AVFormatContext* fmt, const std::string& url)
{
    AVIOContext* ctx = openStream(url);
    if (!ctx) return false;
    fmt->pb = ctx;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    fmt->io_open = hookIoOpen;
    fmt->io_close2 = hookIoClose;
    return true;
}

void detachHttpIo(AVIOContext* pb) { closeStream(pb); }

void abortHttpIo(AVFormatContext* fmt)
{
    (void)fmt;
    std::lock_guard<std::mutex> lock(registry().mutex);
    for (IoStream* io : registry().live) io->stream.abort();
}

} // namespace player
