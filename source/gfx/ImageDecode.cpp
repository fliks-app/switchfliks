#include "gfx/ImageDecode.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace gfx {

namespace {

struct MemSource {
    const uint8_t* data;
    size_t size;
    size_t pos;
};

int memRead(void* opaque, uint8_t* buf, int bufSize)
{
    auto* src = static_cast<MemSource*>(opaque);
    const size_t remaining = src->size - src->pos;
    if (remaining == 0) return AVERROR_EOF;
    const size_t n = std::min(static_cast<size_t>(bufSize), remaining);
    std::memcpy(buf, src->data + src->pos, n);
    src->pos += n;
    return static_cast<int>(n);
}

int64_t memSeek(void* opaque, int64_t offset, int whence)
{
    auto* src = static_cast<MemSource*>(opaque);
    if (whence == AVSEEK_SIZE) return static_cast<int64_t>(src->size);
    int64_t target = offset;
    if (whence == SEEK_CUR) target += static_cast<int64_t>(src->pos);
    else if (whence == SEEK_END) target += static_cast<int64_t>(src->size);
    if (target < 0 || target > static_cast<int64_t>(src->size)) return -1;
    src->pos = static_cast<size_t>(target);
    return target;
}

// Everything ffmpeg hands back has to be released on every exit path, and the
// AVIO buffer is reallocated behind our back, so it is read off the context
// rather than remembered.
struct DecodeScope {
    AVFormatContext* fmt = nullptr;
    AVIOContext* avio = nullptr;
    AVCodecContext* dec = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* rgb = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* sws = nullptr;

    ~DecodeScope()
    {
        if (sws) sws_freeContext(sws);
        if (rgb) av_frame_free(&rgb);
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (dec) avcodec_free_context(&dec);
        if (fmt) {
            AVIOContext* pb = fmt->pb;
            avformat_close_input(&fmt);
            avio = pb;
        }
        if (avio) {
            av_freep(&avio->buffer);
            avio_context_free(&avio);
        }
    }
};

} // namespace

bool decodeImage(const uint8_t* data, size_t size, int maxWidth, DecodedImage& out)
{
    if (!data || size < 16) return false;

    MemSource src{ data, size, 0 };
    DecodeScope scope;

    constexpr int kBufSize = 32768;
    auto* ioBuf = static_cast<unsigned char*>(av_malloc(kBufSize));
    if (!ioBuf) return false;

    scope.avio = avio_alloc_context(ioBuf, kBufSize, 0, &src, memRead, nullptr, memSeek);
    if (!scope.avio) {
        av_free(ioBuf);
        return false;
    }

    scope.fmt = avformat_alloc_context();
    if (!scope.fmt) return false;
    scope.fmt->pb = scope.avio;
    scope.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_open_input(&scope.fmt, nullptr, nullptr, nullptr) != 0) {
        // open_input frees the context on failure but leaves the AVIO to us.
        scope.fmt = nullptr;
        return false;
    }
    if (avformat_find_stream_info(scope.fmt, nullptr) < 0) return false;

    const int streamIndex = av_find_best_stream(scope.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIndex < 0) return false;

    AVStream* stream = scope.fmt->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) return false;

    scope.dec = avcodec_alloc_context3(codec);
    if (!scope.dec) return false;
    if (avcodec_parameters_to_context(scope.dec, stream->codecpar) < 0) return false;
    scope.dec->thread_count = 1;
    if (avcodec_open2(scope.dec, codec, nullptr) < 0) return false;

    scope.frame = av_frame_alloc();
    scope.pkt = av_packet_alloc();
    if (!scope.frame || !scope.pkt) return false;

    bool got = false;
    while (!got && av_read_frame(scope.fmt, scope.pkt) >= 0) {
        if (scope.pkt->stream_index == streamIndex &&
            avcodec_send_packet(scope.dec, scope.pkt) >= 0)
            got = avcodec_receive_frame(scope.dec, scope.frame) >= 0;
        av_packet_unref(scope.pkt);
    }
    if (!got) {
        avcodec_send_packet(scope.dec, nullptr);
        got = avcodec_receive_frame(scope.dec, scope.frame) >= 0;
    }
    if (!got || scope.frame->width <= 0 || scope.frame->height <= 0) return false;

    int dstW = scope.frame->width;
    int dstH = scope.frame->height;
    if (maxWidth > 0 && dstW > maxWidth) {
        dstH = std::max(1, scope.frame->height * maxWidth / scope.frame->width);
        dstW = maxWidth;
    }

    scope.sws = sws_getContext(scope.frame->width, scope.frame->height,
                               static_cast<AVPixelFormat>(scope.frame->format), dstW, dstH,
                               AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!scope.sws) return false;

    scope.rgb = av_frame_alloc();
    if (!scope.rgb) return false;
    scope.rgb->format = AV_PIX_FMT_RGBA;
    scope.rgb->width = dstW;
    scope.rgb->height = dstH;
    if (av_frame_get_buffer(scope.rgb, 32) < 0) return false;

    sws_scale(scope.sws, scope.frame->data, scope.frame->linesize, 0, scope.frame->height,
              scope.rgb->data, scope.rgb->linesize);

    out.width = dstW;
    out.height = dstH;
    out.rgba.resize(static_cast<size_t>(dstW) * dstH * 4);
    for (int y = 0; y < dstH; y++)
        std::memcpy(out.rgba.data() + static_cast<size_t>(y) * dstW * 4,
                    scope.rgb->data[0] + static_cast<size_t>(y) * scope.rgb->linesize[0],
                    static_cast<size_t>(dstW) * 4);
    return true;
}

} // namespace gfx
