#include "player/Player.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <vector>

#include <sys/stat.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
}

#include <switch.h>

#include "net/Api.h"
#include "net/Http.h"
#include "player/AudioOut.h"
#include "player/HlsFeed.h"
#include "player/HttpIo.h"
#include "gfx/Renderer.h"
#include "util/Config.h"
#include "util/Json.h"
#include "util/Log.h"
#include "util/Thread.h"

namespace player {

namespace {

Settings g_settings;
std::mutex g_settingsMutex;

// ISO 639-2/B, the codes ffmpeg reports for a matroska track. Only the
// languages a library is likely to carry; anything else shows its raw tag,
// which is still more use than a stream number.
std::string languageName(const std::string& code)
{
    static const struct {
        const char* code;
        const char* name;
    } kNames[] = {
        { "eng", "English" },   { "fre", "French" },    { "fra", "French" },
        { "ger", "German" },    { "deu", "German" },    { "spa", "Spanish" },
        { "ita", "Italian" },   { "por", "Portuguese" },{ "dut", "Dutch" },
        { "nld", "Dutch" },     { "rus", "Russian" },   { "jpn", "Japanese" },
        { "chi", "Chinese" },   { "zho", "Chinese" },   { "kor", "Korean" },
        { "ara", "Arabic" },    { "pol", "Polish" },    { "swe", "Swedish" },
        { "nor", "Norwegian" }, { "dan", "Danish" },    { "fin", "Finnish" },
        { "tur", "Turkish" },   { "heb", "Hebrew" },    { "hin", "Hindi" },
        { "ces", "Czech" },     { "cze", "Czech" },     { "hun", "Hungarian" },
        { "ell", "Greek" },     { "gre", "Greek" },     { "ukr", "Ukrainian" },
        { "tha", "Thai" },      { "vie", "Vietnamese" },{ "ind", "Indonesian" },
        { "ron", "Romanian" },  { "rum", "Romanian" },  { "cat", "Catalan" },
    };
    for (const auto& entry : kNames)
        if (code == entry.code) return entry.name;
    return code;
}

std::string codecName(const std::string& codec)
{
    if (codec == "aac") return "AAC";
    if (codec == "ac3") return "Dolby Digital";
    if (codec == "eac3") return "Dolby Digital+";
    if (codec == "dts") return "DTS";
    if (codec == "truehd") return "TrueHD";
    if (codec == "flac") return "FLAC";
    if (codec == "mp3") return "MP3";
    if (codec == "opus") return "Opus";
    if (codec == "vorbis") return "Vorbis";
    if (codec == "pcm_s16le" || codec == "pcm_s24le") return "PCM";
    return codec;
}

constexpr const char* kSettingsDir = "sdmc:/switch/fliks";
constexpr const char* kSettingsPath = "sdmc:/switch/fliks/settings.json";

// About a second of decoded video at 24fps. Four frames — which is what the
// packet queue first shipped with — is a sixth of a second, too thin for a
// render stall or a decode spike to be absorbed rather than dropped.
// 24 NV12 frames of 720p is roughly 33 MiB, which this has to spare.
constexpr size_t kDefaultQueuedFrames = 24;
// Two seconds of 48kHz stereo s16 held ahead. One second was the old figure
// and it is not enough: a stream served over HTTP stalls for longer than that
// on a dropped connection, and audio is only 192 KB a second — the cheapest
// buffering in the pipeline and the one the clock depends on.
constexpr size_t kAudioHighWater = 48000 * 4 * 2;
// Undecoded video packets are a few KB each, so holding a whole fragment's
// worth costs about a megabyte. Decoded frames cost 1.4 MB *each*, which is
// why the queue that has to span a fragment is the packet one: these
// fragments carry all of their video samples before any audio, so audio is
// only reachable by reading past every video packet first.
constexpr size_t kMaxPendingPackets = 300;
// Ceiling on decoded video held at once, which is what actually matters —
// a frame is 1.4 MB at 720p and 2.2 MB at 1920x800, so this is 21 frames
// there and the full 24 at 720p.
constexpr size_t kMaxQueuedBytes = 48 * 1024 * 1024;
constexpr double kMaxLateSeconds = 0.08;
// Long enough to outlast audout priming plus decoder latency after a seek.
constexpr double kAudioStallGrace = 1.5;

// Carried on AVCodecContext::opaque so the format callback can see which
// hardware format to hold out for.
struct HwState {
    AVPixelFormat pixFmt = AV_PIX_FMT_NONE;
};

AVPixelFormat pickHwFormat(AVCodecContext* ctx, const AVPixelFormat* formats)
{
    const auto* hw = static_cast<const HwState*>(ctx->opaque);
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; p++)
        if (*p == hw->pixFmt) return *p;
    // The decoder offered nothing we asked for; software output is still fine.
    return formats[0];
}

} // namespace

int qualityHeight(const std::string& id)
{
    if (id.empty() || id == "auto" || id == "original") return 0;
    const size_t at = id.rfind('-');
    const char* digits = id.c_str() + (at == std::string::npos ? 0 : at + 1);
    const int h = std::atoi(digits);
    return h > 0 ? h : 0;
}

int64_t qualityBitrate(const std::string& id)
{
    for (const api::QualityRung& rung : api::knownQualities())
        if (rung.id == id) return rung.bitrateBps;
    return 0;
}

std::string AudioTrack::label() const
{
    // Language first: it is what anyone is actually choosing between, and the
    // codec only disambiguates two tracks in the same language.
    std::string out = languageName(language);
    if (!title.empty() && title != out) out = title;
    if (out.empty()) out = "Track " + std::to_string(index);
    if (!codec.empty()) out += " · " + codecName(codec);
    if (channels == 1) out += " mono";
    else if (channels == 2) out += " stereo";
    else if (channels == 6) out += " 5.1";
    else if (channels == 8) out += " 7.1";
    else if (channels > 0) out += " " + std::to_string(channels) + "ch";
    return out;
}

Settings settings()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    return g_settings;
}

std::vector<AudioTrack> Player::audioTracks() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_audioTracks;
}

void Player::selectAudioTrack(int streamIndex)
{
    if (streamIndex == m_audioTrack.load()) return;
    m_audioTrackRequest = streamIndex;
    m_cv.notify_all();
}

void loadSettings()
{
    FILE* f = std::fopen(kSettingsPath, "rb");
    if (!f) return;
    std::string text;
    char buf[512];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
    if (text.empty()) return;

    const util::Json root = util::Json::parse(text);
    if (!root.valid()) return;

    std::lock_guard<std::mutex> lock(g_settingsMutex);
    // Each key is taken only if present, so a file written by an older build
    // keeps its defaults for whatever it did not know about.
    if (root["quality"].hasStr()) g_settings.quality = root["quality"].str();
    if (root["showStats"].valid()) g_settings.showStats = root["showStats"].boolean();
    FLIKS_LOG("settings: quality=%s stats=%d", g_settings.quality.c_str(),
              g_settings.showStats ? 1 : 0);
}

void setSettings(const Settings& s)
{
    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        g_settings = s;
    }
    // The write happens outside the lock. settings() is read once per frame by
    // the draw loop, and an SD-card write is slow enough that holding the
    // mutex across it would stall rendering.
    //
    // Rung ids are [a-z0-9-], but nothing stops a future key from carrying
    // something that needs quoting, so the value is escaped rather than
    // trusted.
    std::string quality;
    for (char ch : s.quality) {
        if (ch == '"' || ch == '\\') quality.push_back('\\');
        if (static_cast<unsigned char>(ch) >= 0x20) quality.push_back(ch);
    }
    const std::string payload = "{\"quality\":\"" + quality + "\",\"showStats\":" +
                                (s.showStats ? "true" : "false") + "}";
    ::mkdir("sdmc:/switch", 0777);
    ::mkdir(kSettingsDir, 0777);
    if (FILE* f = std::fopen(kSettingsPath, "wb")) {
        std::fwrite(payload.data(), 1, payload.size(), f);
        std::fclose(f);
    }
}

struct Player::Impl {
    AVFormatContext* fmt = nullptr;
    AVBufferRef* hwDevice = nullptr;
    AVFrame* swFrame = nullptr;   // download target for hardware frames
    HwState hw;
    HlsFeed feed;
    bool usingFeed = false;
    int64_t streamBitrate = 0;
    AVCodecContext* video = nullptr;
    AVCodecContext* audio = nullptr;
    SwrContext* swr = nullptr;
    AudioOut out;
    int videoStream = -1;
    int audioStream = -1;
    double videoTimeBase = 0;
    double audioTimeBase = 0;
    std::vector<uint8_t> audioBuffer;
};

Player::Player() : m_impl(new Impl) {}

Player::~Player()
{
    close();
    delete m_impl;
}

std::string Player::error() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_error;
}

void Player::fail(const std::string& message)
{
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_error = message;
    }
    m_state = State::Failed;
}

double Player::position() const
{
    if (m_state.load() == State::Idle) return 0.0;
    // Audio is the master clock. m_clockBase is the stream time that audout's
    // zero corresponds to, and it survives a fallback untouched — so coming
    // back to audio lands on the truth rather than on wherever the wall
    // clock had wandered to.
    if (m_useAudioClock.load()) return m_clockBase.load() + m_impl->out.clock();
    return m_wallBase.load() + m_fallbackClock.load();
}

void Player::advanceClock(double dt)
{
    if (m_paused.load() || m_state.load() != State::Playing) return;

    const double audio = m_impl->out.clock();

    if (m_useAudioClock.load()) {
        if (audio > m_lastAudioClock.load() + 1e-6) {
            m_lastAudioClock = audio;
            m_audioStallSeconds = 0.0;
            return;
        }
        m_audioStallSeconds = m_audioStallSeconds.load() + dt;
        // audout primes with about a quarter second of buffers before it
        // retires anything real, and a seek restarts that — so the grace has
        // to outlast priming or every seek drops out of sync.
        if (m_audioStallSeconds.load() > kAudioStallGrace && queuedFrames() > 0) {
            FLIKS_LOG("player: audio clock stuck at %.3f after %.2fs, switching to the wall clock",
                      audio, m_audioStallSeconds.load());
            m_wallBase = m_clockBase.load() + audio;
            m_fallbackClock = 0.0;
            m_useAudioClock = false;
        }
        return;
    }

    m_fallbackClock.store(m_fallbackClock.load() + dt);

    // A wall clock and an audio device do not keep the same time, so staying
    // on the fallback once audio recovers guarantees drift. Going back re-locks
    // video to what is actually being heard.
    if (m_audioAvailable.load() && audio > m_lastAudioClock.load() + 1e-6) {
        FLIKS_LOG("player: audio clock recovered at %.3f, re-locking", audio);
        m_lastAudioClock = audio;
        m_audioStallSeconds = 0.0;
        m_useAudioClock = true;
    }
}

int Player::queuedFrames() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_frames.size());
}

double Player::bufferedSeconds() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_frames.empty()) return 0.0;
    return static_cast<double>(m_frames.size()) / 24.0;
}

bool Player::open(const std::string& url, double startAtSeconds)
{
    close();

    m_state = State::Opening;
    m_clockBase = 0.0;
    m_pendingStartSeek = startAtSeconds > 0.5 ? startAtSeconds : -1.0;
    m_clockAnchored = false;
    m_useAudioClock = false;
    m_audioAvailable = false;
    m_wallBase = 0.0;
    m_fallbackClock = 0.0;
    m_lastAudioClock = 0.0;
    m_audioStallSeconds = 0.0;
    m_lastPts = startAtSeconds;
    m_dropped = 0;
    m_seekRequest = -1.0;
    m_paused = false;
    // A new title picks its own default track; only a restart within one
    // title carries a choice across, and that path does not come through here.
    m_audioTrack = -1;
    m_audioTrackRequest = -1;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_audioTracks.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_error.clear();
    }

    m_openUrl = url;
    m_running = true;
    if (!m_thread.start([this] { demuxLoop(); }, util::Thread::kDemuxStack)) {
        m_running = false;
        fail("could not start the decoder");
        return false;
    }
    return true;
}

void Player::close()
{
    if (!m_running.exchange(false)) {
        clearFrames();
        m_state = State::Idle;
        return;
    }
    abortHttpIo(nullptr);
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    clearFrames();
    m_state = State::Idle;
}

void Player::clearFrames()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (AVFrame* frame : m_frames) av_frame_free(&frame);
    m_frames.clear();
}

void Player::setPaused(bool paused)
{
    m_paused = paused;
    m_impl->out.setPaused(paused);
    if (m_state.load() == State::Playing || m_state.load() == State::Paused)
        m_state = paused ? State::Paused : State::Playing;
    m_cv.notify_all();
}

void Player::seekTo(double seconds)
{
    m_seekRequest = std::max(0.0, seconds);
    m_cv.notify_all();
}

void Player::seekBy(double delta) { seekTo(position() + delta); }

AVFrame* Player::acquireFrame()
{
    const double clock = position();
    std::unique_lock<std::mutex> lock(m_mutex);

    AVFrame* chosen = nullptr;
    while (!m_frames.empty()) {
        AVFrame* front = m_frames.front();
        const double pts = front->pts != AV_NOPTS_VALUE
                               ? front->pts * m_impl->videoTimeBase
                               : clock;
        if (pts > clock + 0.001) break;

        m_frames.pop_front();
        if (chosen) {
            // A frame we never showed: the decoder fell behind real time.
            av_frame_free(&chosen);
            m_dropped.fetch_add(1);
        }
        chosen = front;
        if (clock - pts < kMaxLateSeconds) break;
    }

    lock.unlock();
    m_cv.notify_all();
    return chosen;
}

void Player::releaseFrame(AVFrame* frame)
{
    if (frame) av_frame_free(&frame);
}

int Player::interruptCb(void* opaque)
{
    // Non-zero aborts whatever ffmpeg is blocked on, so close() does not have
    // to wait out a stalled segment fetch.
    auto* self = static_cast<Player*>(opaque);
    return self->m_running.load() ? 0 : 1;
}

void Player::demuxLoop()
{
    double startAt = m_pendingStartSeek.load();
    if (startAt < 0.0) startAt = 0.0;

    while (m_running.load()) {
        m_restartPending = false;
        runSession(startAt);
        if (!m_restartPending.load()) break;
        startAt = m_restartAt.load();
        // A restart is a new timeline. The clock anchor is only cleared by
        // open() and by the in-place seek, so a fed restart used to carry the
        // previous session's base across: the first frame of the new segment
        // never re-anchored, position() kept reporting where the old session
        // was, and since every new frame's PTS was ahead of it nothing was
        // ever due to show. The picture froze on the last frame while the bar
        // carried on — decode 0.0ms, queue pinned at full, in the log.
        clearFrames();
        m_clockAnchored = false;
        m_clockBase = startAt;
        m_wallBase = startAt;
        m_fallbackClock = 0.0;
        m_lastAudioClock = 0.0;
        m_audioStallSeconds = 0.0;
        m_useAudioClock = false;
        FLIKS_LOG("player: restarting feed at %.2f", startAt);
    }
}

void Player::runSession(double startAt)
{
    Impl& impl = *m_impl;

    // Every exit below funnels through here: the custom AVIO is ours, so
    // avformat never frees it, and the audio thread must be stopped before
    // the decoder it pulls from goes away.
    auto teardown = [&] {
        impl.out.stop();
        if (impl.swFrame) av_frame_free(&impl.swFrame);
        if (impl.swr) swr_free(&impl.swr);
        if (impl.video) avcodec_free_context(&impl.video);
        if (impl.audio) avcodec_free_context(&impl.audio);
        if (impl.hwDevice) av_buffer_unref(&impl.hwDevice);
        impl.hw.pixFmt = AV_PIX_FMT_NONE;
        if (impl.fmt) {
            AVIOContext* pb = impl.fmt->pb;
            avformat_close_input(&impl.fmt);
            if (impl.usingFeed) detachFeedIo(pb);
            else detachHttpIo(pb);
        }
        impl.feed.close();
        impl.fmt = nullptr;
    };

    impl.fmt = avformat_alloc_context();
    if (!impl.fmt) {
        fail("out of memory");
        return;
    }

    const std::string url = m_openUrl;

    // A playlist is walked here rather than by ffmpeg's HLS demuxer, which
    // wedged after every segment boundary: a fragment would download in full,
    // hit EOF and yield no packets.
    impl.usingFeed = url.find(".m3u8") != std::string::npos;
    if (impl.usingFeed) {
        net::Request req;
        req.url = url;
        const net::Response body = net::perform(req);
        if (!body.ok() || !impl.feed.parse(url, body.body)) {
            fail("could not read the playlist");
            avformat_free_context(impl.fmt);
            impl.fmt = nullptr;
            return;
        }
        m_pendingStartSeek = -1.0;
        if (!impl.feed.open(startAt)) {
            fail("could not fetch the first segment");
            avformat_free_context(impl.fmt);
            impl.fmt = nullptr;
            return;
        }
        // Fragments carry absolute timestamps, so the clock is anchored to
        // the segment the feed actually started on.
        m_clockBase = impl.feed.startTime();
        m_duration = impl.feed.totalDuration();
        if (!attachFeedIo(impl.fmt, &impl.feed)) {
            fail("out of memory");
            avformat_free_context(impl.fmt);
            impl.fmt = nullptr;
            return;
        }
    } else if (!attachHttpIo(impl.fmt, url)) {
        fail("could not reach the stream");
        avformat_free_context(impl.fmt);
        impl.fmt = nullptr;
        return;
    }

    impl.fmt->interrupt_callback.callback = &Player::interruptCb;
    impl.fmt->interrupt_callback.opaque = this;

    // The HLS demuxer validates every child URL against protocol_whitelist
    // before it will call io_open, and a context opened with custom IO never
    // gets a default one — so the variant playlist is rejected and the open
    // stalls out. allowed_extensions covers segments served without a
    // recognised suffix, which Fliks does when the URL carries a token.
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "protocol_whitelist", "file,http,https,tcp,tls,crypto,data", 0);
    av_dict_set(&opts, "allowed_extensions", "ALL", 0);
    av_dict_set(&opts, "max_reload", "8", 0);

    AVIOContext* ownedIo = impl.fmt->pb;
    FLIKS_LOG("player: opening %s", util::loggableUrl(url).c_str());
    const int openRc = avformat_open_input(&impl.fmt, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (openRc != 0) {
        char err[128] = { 0 };
        av_strerror(openRc, err, sizeof(err));
        FLIKS_LOG("player: avformat_open_input failed: %s", err);
        // open_input frees the context on failure but never the custom IO.
        fail("unsupported stream");
        impl.fmt = nullptr;
        detachHttpIo(ownedIo);
        return;
    }
    FLIKS_LOG("player: probing streams");
    if (avformat_find_stream_info(impl.fmt, nullptr) < 0) {
        fail("could not read the stream");
        teardown();
        return;
    }
    impl.streamBitrate = impl.fmt->bit_rate;
    if (impl.streamBitrate > 0)
        FLIKS_LOG("player: stream bitrate %.2f Mbit/s (%.0f KiB/s sustained)",
                  impl.streamBitrate / 1.0e6, impl.streamBitrate / 8.0 / 1024.0);
    FLIKS_LOG("player: opened '%s', %u streams, duration %.1fs", impl.fmt->iformat->name,
              impl.fmt->nb_streams, static_cast<double>(impl.fmt->duration) / AV_TIME_BASE);

    if (!impl.usingFeed && impl.fmt->duration > 0)
        m_duration = static_cast<double>(impl.fmt->duration) / AV_TIME_BASE;

    impl.videoStream = av_find_best_stream(impl.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    impl.audioStream = av_find_best_stream(impl.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    // The lookup reports "not found" as an error code, not as -1; keeping the
    // raw value made a missing audio track read as a huge negative index.
    if (impl.audioStream < 0) impl.audioStream = -1;
    if (impl.videoStream < 0) {
        fail("no video track");
        teardown();
        return;
    }

    // Software decode on four A57s has a hard ceiling. Allocating a decoder
    // above it does not merely stutter, it exhausts the frame pool and takes
    // the process down, so it is refused with something the user can read.
    {
        const AVCodecParameters* par = impl.fmt->streams[impl.videoStream]->codecpar;
        if (par->width > 1920 || par->height > 1088) {
            FLIKS_LOG("player: refusing %dx%d, beyond the software decode ceiling", par->width,
                      par->height);
            fail("this stream is too large to decode on this hardware");
            teardown();
            return;
        }
    }

    auto openDecoder = [&](int index, AVCodecContext** out) -> bool {
        AVStream* stream = impl.fmt->streams[index];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) return false;
        *out = avcodec_alloc_context3(codec);
        if (!*out) return false;
        if (avcodec_parameters_to_context(*out, stream->codecpar) < 0) {
            avcodec_free_context(out);
            return false;
        }
        // The X1's NVDEC block, via averne's nvtegra backend — devkitPro's
        // ffmpeg is built with --enable-nvtegra. Looked up by name rather
        // than by enum so a toolchain without it still compiles and simply
        // decodes in software.
        if (index == impl.videoStream && util::configInt("hwdec", 1) != 0) {
            const AVHWDeviceType type = av_hwdevice_find_type_by_name("nvtegra");
            if (type != AV_HWDEVICE_TYPE_NONE &&
                av_hwdevice_ctx_create(&impl.hwDevice, type, nullptr, nullptr, 0) >= 0) {
                impl.hw.pixFmt = av_get_pix_fmt("nvtegra");
                (*out)->hw_device_ctx = av_buffer_ref(impl.hwDevice);
                (*out)->opaque = &impl.hw;
                (*out)->get_format = pickHwFormat;
                FLIKS_LOG("player: nvtegra hardware decoding requested");
            } else {
                FLIKS_LOG("player: no nvtegra device, decoding in software");
            }
        }

        // Only relevant to the software path; harmless otherwise.
        (*out)->thread_count = util::configInt("decodeThreads", 3);
        (*out)->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        // Deblocking is the single most expensive stage left once threading
        // is on, and dropping it trades some blockiness for roughly a fifth
        // of the decode budget back.
        if (util::configInt("skipLoopFilter", 0) != 0)
            (*out)->skip_loop_filter = AVDISCARD_ALL;
        if (avcodec_open2(*out, codec, nullptr) != 0) {
            avcodec_free_context(out);
            return false;
        }
        return true;
    };

    if (!openDecoder(impl.videoStream, &impl.video)) {
        fail("no decoder for this video");
        teardown();
        return;
    }
    impl.videoTimeBase = av_q2d(impl.fmt->streams[impl.videoStream]->time_base);
    m_videoWidth = impl.video->width;
    m_videoHeight = impl.video->height;
    FLIKS_LOG("player: video %s %dx%d pixfmt=%d, audio stream %d",
              avcodec_get_name(impl.video->codec_id), impl.video->width, impl.video->height,
              static_cast<int>(impl.video->pix_fmt), impl.audioStream);

    {
        // Every audio stream the container carries, in its own order, so a
        // switch can name one by ffmpeg's index rather than a position that
        // shifts when the session is rebuilt.
        std::vector<AudioTrack> tracks;
        for (unsigned i = 0; i < impl.fmt->nb_streams; i++) {
            const AVStream* st = impl.fmt->streams[i];
            if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
            AudioTrack track;
            track.index = static_cast<int>(i);
            if (const AVDictionaryEntry* e = av_dict_get(st->metadata, "language", nullptr, 0))
                track.language = e->value;
            if (const AVDictionaryEntry* e = av_dict_get(st->metadata, "title", nullptr, 0))
                track.title = e->value;
            if (const char* name = avcodec_get_name(st->codecpar->codec_id)) track.codec = name;
            track.channels = st->codecpar->ch_layout.nb_channels;
            tracks.push_back(std::move(track));
        }
        // A track chosen before this session started — a switch reopens the
        // whole thing on a fed stream — is honoured if it is still there.
        const int wanted = m_audioTrack.load();
        if (wanted >= 0) {
            for (const AudioTrack& track : tracks)
                if (track.index == wanted) impl.audioStream = wanted;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_audioTracks = std::move(tracks);
    }
    m_audioTrack = impl.audioStream;
    for (const AudioTrack& track : audioTracks())
        FLIKS_LOG("player: audio track %d: %s", track.index, track.label().c_str());

    // audout is fixed at 48kHz stereo s16, so every track resamples to that
    // whatever it carries.
    auto buildResampler = [&] {
        if (impl.swr) swr_free(&impl.swr);
        impl.swr = nullptr;
        if (!impl.audio) return;
        AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&impl.swr, &outLayout, AV_SAMPLE_FMT_S16, AudioOut::kSampleRate,
                                &impl.audio->ch_layout, impl.audio->sample_fmt,
                                impl.audio->sample_rate, 0, nullptr) < 0 ||
            swr_init(impl.swr) < 0) {
            swr_free(&impl.swr);
            impl.swr = nullptr;
        }
    };

    if (impl.audioStream >= 0 && openDecoder(impl.audioStream, &impl.audio)) {
        impl.audioTimeBase = av_q2d(impl.fmt->streams[impl.audioStream]->time_base);
        buildResampler();
    }
    // Only trust the audio clock if there is really audio behind it.
    const bool audioReady = impl.audio && impl.swr && impl.out.start();
    m_useAudioClock = audioReady;
    m_audioAvailable = audioReady;
    m_lastAudioClock = 0.0;
    m_audioStallSeconds = 0.0;
    FLIKS_LOG("player: audio %s", audioReady ? "ready" : "unavailable, using the wall clock");

    FLIKS_LOG("player: decoding");
    m_state = State::Playing;

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    // The decode thread owns this one; `frame` stays with audio on this thread.
    AVFrame* vframe = av_frame_alloc();
    bool eof = false;
    int packetsSeen = 0;
    int framesDecoded = 0;
    size_t softQueuedFrames =
        static_cast<size_t>(util::configInt("videoQueue", kDefaultQueuedFrames));
    // Hold a budget of bytes rather than a count of frames: 24 frames is a
    // sensible depth at 720p and 75 MB at 1080p, and allocating and freeing
    // that much per second contends the allocator lock with every other
    // thread — including the one feeding audio.
    if (impl.video->width > 0 && impl.video->height > 0) {
        const size_t frameBytes =
            static_cast<size_t>(impl.video->width) * impl.video->height * 3 / 2;
        const size_t budget = kMaxQueuedBytes / std::max<size_t>(frameBytes, 1);
        softQueuedFrames = std::max<size_t>(4, std::min(softQueuedFrames, budget));
        FLIKS_LOG("player: video queue %zu frames (%zu KiB each)", softQueuedFrames,
                  frameBytes / 1024);
    }

    // Video decode moved off this thread. It is 4.4ms a frame at 1080p, and
    // this loop reads one packet per pass — so with the ten-or-so video
    // packets that separate two audio packets, an audio packet worth 21ms of
    // sound was arriving every 40ms. The device ran dry at twice the rate it
    // was fed, which is exactly the underrun count the log reported. Reading
    // and audio now never wait on a video frame.
    struct VideoPump {
        std::mutex mutex;
        std::condition_variable cv;      // producer and state changes -> decoder
        std::condition_variable idleCv;  // decoder -> producer, for the handshake
        std::deque<AVPacket*> queue;
        std::atomic<size_t> depth{ 0 };  // readable without the lock, for logging
        bool stop = false;
        bool suspend = false;  // decoder must park and stay out of the codec
        bool idle = false;     // true only while parked in the wait loop
        bool drain = false;    // end of stream: flush the decoder once
        bool drained = false;
    } pump;
    int audioPacketsSeen = 0;
    int audioWritesSeen = 0;
    // The hardware decoder hands back frames in device memory; getting them
    // into something uploadable means a detile. At 1080p that is the single
    // biggest per-frame cost left, so it is measured rather than assumed.
    // Written by the decode thread, read and reset by the demuxer, which is
    // where the periodic line is emitted from now.
    std::atomic<uint64_t> transferTicks{ 0 };
    std::atomic<uint64_t> decodeTicks{ 0 };
    std::atomic<int> timedFrames{ 0 };
    uint64_t lastStatTick = armGetSystemTick();
    int64_t lastWrittenBytes = 0;
    int64_t lastIoBytes = net::bytesRead();
    int starvedWindows = 0;
    bool bitrateWarned = false;

    // Callers hold the pump suspended or stopped.
    auto releasePending = [&pump] {
        std::lock_guard<std::mutex> lock(pump.mutex);
        for (AVPacket* p : pump.queue) {
            AVPacket* tmp = p;
            av_packet_free(&tmp);
        }
        pump.queue.clear();
        pump.depth = 0;
        pump.drain = false;
        pump.drained = false;
    };

    // `queuedPacket` may be null, which flushes the decoder at end of stream.
    auto decodeVideo = [&](AVPacket* queuedPacket) {
        const uint64_t decodeStart = armGetSystemTick();
        if (avcodec_send_packet(impl.video, queuedPacket) < 0) return;
        while (avcodec_receive_frame(impl.video, vframe) >= 0) {
            AVFrame* source = vframe;
            if (impl.hw.pixFmt != AV_PIX_FMT_NONE && vframe->format == impl.hw.pixFmt) {
                if (!impl.swFrame) impl.swFrame = av_frame_alloc();
                const uint64_t transferStart = armGetSystemTick();
                const int transferRc = av_hwframe_transfer_data(impl.swFrame, vframe, 0);
                transferTicks.fetch_add(armGetSystemTick() - transferStart);
                if (transferRc < 0) {
                    FLIKS_LOG("player: hardware frame download failed");
                    av_frame_unref(vframe);
                    continue;
                }
                av_frame_copy_props(impl.swFrame, vframe);
                av_frame_unref(vframe);
                source = impl.swFrame;
            }
            if (framesDecoded < 5) {
                framesDecoded++;
                FLIKS_LOG("decoded video frame %d: %dx%d fmt=%d%s", framesDecoded, source->width,
                          source->height, source->format,
                          source == impl.swFrame ? " (hw)" : "");
            }
            AVFrame* copy = av_frame_alloc();
            av_frame_move_ref(copy, source);
            m_videoWidth = copy->width;
            m_videoHeight = copy->height;
            if (copy->pts != AV_NOPTS_VALUE) {
                const double pts = copy->pts * impl.videoTimeBase;
                m_lastPts = pts;
                if (!m_clockAnchored.exchange(true)) {
                    m_clockBase = pts;
                    m_wallBase = pts;
                    FLIKS_LOG("player: first frame pts %.3f, clock=%s", pts,
                              m_useAudioClock.load() ? "audio" : "wall");
                }
            } else if (!m_clockAnchored.exchange(true)) {
                FLIKS_LOG("player: first frame has no pts, clock=%s",
                          m_useAudioClock.load() ? "audio" : "wall");
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_frames.push_back(copy);
            }
            const double pending = m_pendingStartSeek.exchange(-1.0);
            if (pending >= 0.0) {
                FLIKS_LOG("player: applying resume offset %.2f", pending);
                m_seekRequest = pending;
            }

            timedFrames.fetch_add(1);
        }
        decodeTicks.fetch_add(armGetSystemTick() - decodeStart);
    };

    auto pumpLoop = [&] {
        for (;;) {
            AVPacket* pkt = nullptr;
            bool flushNow = false;
            {
                std::unique_lock<std::mutex> lock(pump.mutex);
                for (;;) {
                    if (pump.stop || !m_running.load()) break;
                    if (!pump.suspend) {
                        // Room is measured in decoded frames, which is what
                        // costs memory; packets ahead of it are cheap.
                        const bool room = queuedFrames() < static_cast<int>(softQueuedFrames);
                        if (room && !pump.queue.empty()) {
                            pkt = pump.queue.front();
                            pump.queue.pop_front();
                            pump.depth = pump.queue.size();
                            break;
                        }
                        if (room && pump.drain && !pump.drained && pump.queue.empty()) {
                            flushNow = true;
                            break;
                        }
                    }
                    // `idle` is set true only here, so a producer that sees it
                    // knows this thread is parked and not inside the codec.
                    pump.idle = true;
                    pump.idleCv.notify_all();
                    pump.cv.wait_for(lock, std::chrono::milliseconds(5));
                }
                if (pump.stop || !m_running.load()) break;
                pump.idle = false;
            }
            if (flushNow) {
                decodeVideo(nullptr);
                std::lock_guard<std::mutex> lock(pump.mutex);
                pump.drained = true;
                pump.idleCv.notify_all();
                continue;
            }
            decodeVideo(pkt);
            av_packet_free(&pkt);
        }
        std::lock_guard<std::mutex> lock(pump.mutex);
        pump.idle = true;
        pump.idleCv.notify_all();
    };

    // Park the decoder outside the codec, so buffers can be flushed and the
    // queue cleared without racing it.
    auto suspendPump = [&] {
        std::unique_lock<std::mutex> lock(pump.mutex);
        pump.suspend = true;
        pump.cv.notify_all();
        while (!pump.idle) pump.idleCv.wait_for(lock, std::chrono::milliseconds(5));
    };
    auto resumePump = [&] {
        std::lock_guard<std::mutex> lock(pump.mutex);
        pump.suspend = false;
        pump.idle = false;
        pump.cv.notify_all();
    };

    util::Thread videoThread;
    if (!videoThread.start(pumpLoop, util::Thread::kDemuxStack)) {
        FLIKS_LOG("player: could not start the video decode thread");
        m_state = State::Failed;
        av_frame_free(&vframe);
        av_frame_free(&frame);
        av_packet_free(&packet);
        teardown();
        return;
    }
    auto stopPump = [&] {
        {
            std::lock_guard<std::mutex> lock(pump.mutex);
            pump.stop = true;
            pump.cv.notify_all();
        }
        videoThread.join();
    };

    while (m_running.load()) {
        // A track change swaps one decoder for another, which cannot happen
        // with packets in flight — so it is handled here, at the top of a
        // pass, and then re-anchored by seeking to where playback already is.
        const int wantAudio = m_audioTrackRequest.exchange(-1);
        if (wantAudio >= 0 && wantAudio != impl.audioStream) {
            const double at = position();
            FLIKS_LOG("player: audio track %d -> %d at %.1fs", impl.audioStream, wantAudio, at);
            m_audioTrack = wantAudio;
            if (impl.usingFeed) {
                // A fed stream has no index to seek, so the session is rebuilt
                // — and reopening reads the track back out of m_audioTrack.
                m_restartAt = at;
                m_restartPending = true;
                m_cv.notify_all();
                break;
            }
            if (impl.audio) avcodec_free_context(&impl.audio);
            impl.audioStream = wantAudio;
            if (openDecoder(impl.audioStream, &impl.audio)) {
                impl.audioTimeBase = av_q2d(impl.fmt->streams[impl.audioStream]->time_base);
                buildResampler();
            } else {
                FLIKS_LOG("player: could not open audio track %d", wantAudio);
                buildResampler();
            }
            m_audioAvailable = impl.audio && impl.swr;
            // Seeking back to the current position is what re-anchors the
            // clock: the ring still holds the old track's samples, and the
            // seek path already knows how to flush and re-lock.
            m_seekRequest = at;
        }

        const double seek = m_seekRequest.exchange(-1.0);
        if (seek >= 0.0) {
            FLIKS_LOG("player: seeking to %.2f", seek);
            // On a fed stream the bytes are a concatenation with no index, so
            // seeking means restarting the feed at another segment. The
            // format context is rebuilt around it on the next pass.
            int seekRc = 0;
            if (impl.usingFeed) {
                m_restartAt = seek;
                m_restartPending = true;
                m_cv.notify_all();
                break;
            }
            const int64_t ts = static_cast<int64_t>(seek * AV_TIME_BASE);
            seekRc = av_seek_frame(impl.fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
            FLIKS_LOG("player: av_seek_frame rc=%d", seekRc);
            if (seekRc >= 0) {
                suspendPump();
                avcodec_flush_buffers(impl.video);
                if (impl.audio) avcodec_flush_buffers(impl.audio);
                releasePending();
                clearFrames();
                impl.out.flush();
                m_clockBase = seek;
                m_wallBase = seek;
                m_clockAnchored = false;
                m_fallbackClock = 0.0;
                // out.flush() put the audio clock back to zero; leaving the
                // stall timer running would trip the watchdog immediately.
                m_lastAudioClock = 0.0;
                m_audioStallSeconds = 0.0;
                m_useAudioClock = m_audioAvailable.load();
                eof = false;
                m_state = m_paused.load() ? State::Paused : State::Playing;
                resumePump();
                FLIKS_LOG("player: seek applied");
            }
        }

        if (!m_running.load()) break;

        // Emitted here, on a timer, rather than every 120 decoded frames: a
        // stalled pipeline decodes nothing, so the frame-counted version went
        // silent exactly when there was something to report.
        {
            const uint64_t nowTick = armGetSystemTick();
            const double sinceStat = armTicksToNs(nowTick - lastStatTick) / 1.0e9;
            if (sinceStat >= 2.0) {
                const int frames = timedFrames.exchange(0);
                const double decodeMs =
                    frames > 0 ? armTicksToNs(decodeTicks.exchange(0)) / 1.0e6 / frames : 0.0;
                const double transferMs =
                    frames > 0 ? armTicksToNs(transferTicks.exchange(0)) / 1.0e6 / frames : 0.0;
                if (frames == 0) {
                    decodeTicks = 0;
                    transferTicks = 0;
                }
                const int64_t written = impl.out.writtenBytes();
                const int64_t ioBytes = net::bytesRead();
                // 192000 B/s is break-even for the audio device. Below it the
                // device runs dry however healthy the picture looks.
                const double audioRate = (written - lastWrittenBytes) / sinceStat;
                const double netRate = (ioBytes - lastIoBytes) / sinceStat;
                lastStatTick = nowTick;
                lastWrittenBytes = written;
                lastIoBytes = ioBytes;

                // Said once, plainly: when the link cannot carry the file,
                // every other number below is a symptom and nothing in the
                // decoder or the buffers will change it.
                if (impl.streamBitrate > 0 && netRate > 0.0) {
                    if (netRate * 8.0 < impl.streamBitrate * 0.9) {
                        if (++starvedWindows == 3 && !bitrateWarned) {
                            bitrateWarned = true;
                            FLIKS_LOG("player: the link is delivering %.2f Mbit/s and this "
                                      "stream needs %.2f Mbit/s — direct play cannot keep up. "
                                      "Set maxBitrate in config.txt below the link speed so "
                                      "the server transcodes.",
                                      netRate * 8.0 / 1.0e6, impl.streamBitrate / 1.0e6);
                        }
                    } else {
                        starvedWindows = 0;
                    }
                }

                FLIKS_LOG("player: %dx%d %.1ffps decode %.1fms (transfer %.1fms) queued=%d/%d "
                          "packets=%zu feed=%zuKiB dropped=%d | audio %zuKiB %.0fB/s under=%d "
                          "| net %.0fKiB/s stalled=%lldms",
                          m_videoWidth.load(), m_videoHeight.load(), frames / sinceStat, decodeMs,
                          transferMs, queuedFrames(), static_cast<int>(softQueuedFrames),
                          pump.depth.load(),
                          impl.usingFeed ? impl.feed.bufferedBytes() / 1024 : 0, m_dropped.load(),
                          impl.out.queuedBytes() / 1024, audioRate, impl.out.underruns(),
                          netRate / 1024.0, static_cast<long long>(net::stallMs()));
            }
        }

        // Reading stops only when *both* sides are satisfied. Gating on the
        // decoded-frame queue alone was what starved audio: the clock that
        // releases video frames never ticked, so the picture froze.
        const bool packetsFull = pump.depth.load() >= kMaxPendingPackets;
        const bool audioFull = !audioReady || impl.out.queuedBytes() >= kAudioHighWater;
        if (packetsFull && audioFull) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(5));
            continue;
        }

        if (eof) {
            bool drained;
            {
                std::lock_guard<std::mutex> lock(pump.mutex);
                drained = pump.drained;
            }
            if (drained && queuedFrames() == 0 && impl.out.queuedBytes() == 0) {
                if (!m_restartPending.load()) m_state = State::Ended;
                break;
            }
            svcSleepThread(20'000'000ULL);
            continue;
        }

        const int rc = av_read_frame(impl.fmt, packet);
        packetsSeen++;
        if (packetsSeen <= 12) {
            FLIKS_LOG("pkt %d: rc=%d stream=%d size=%d pts=%lld", packetsSeen, rc,
                      rc >= 0 ? packet->stream_index : -1, rc >= 0 ? packet->size : 0,
                      rc >= 0 ? static_cast<long long>(packet->pts) : 0LL);
        }
        if (rc < 0) {
            // The decode thread finishes what is queued and flushes the codec.
            {
                std::lock_guard<std::mutex> lock(pump.mutex);
                pump.drain = true;
                pump.cv.notify_all();
            }
            if (impl.audio) avcodec_send_packet(impl.audio, nullptr);
            eof = true;
            continue;
        }

        if (packet->stream_index == impl.videoStream) {
            // Buffered rather than decoded here, so audio further down the
            // fragment is reachable without holding a fragment's worth of
            // decoded frames in memory.
            if (AVPacket* clone = av_packet_clone(packet)) {
                std::lock_guard<std::mutex> lock(pump.mutex);
                pump.queue.push_back(clone);
                pump.depth = pump.queue.size();
                pump.cv.notify_all();
            }
            av_packet_unref(packet);
            continue;
        }

        if (packet->stream_index == impl.audioStream && audioPacketsSeen < 3) {
            audioPacketsSeen++;
            FLIKS_LOG("audio pkt %d after %d video packets, size=%d pts=%lld", audioPacketsSeen,
                      packetsSeen, packet->size, static_cast<long long>(packet->pts));
        }

        if (packet->stream_index == impl.audioStream && impl.audio &&
            avcodec_send_packet(impl.audio, packet) >= 0) {
            while (avcodec_receive_frame(impl.audio, frame) >= 0) {
                if (!impl.swr) {
                    av_frame_unref(frame);
                    continue;
                }
                const int maxOut = swr_get_out_samples(impl.swr, frame->nb_samples);
                if (maxOut <= 0) {
                    av_frame_unref(frame);
                    continue;
                }
                impl.audioBuffer.resize(static_cast<size_t>(maxOut) * AudioOut::kChannels * 2);
                uint8_t* dst = impl.audioBuffer.data();
                const int converted =
                    swr_convert(impl.swr, &dst, maxOut,
                                const_cast<const uint8_t**>(frame->data), frame->nb_samples);
                if (converted > 0) {
                    if (audioWritesSeen < 3) {
                        audioWritesSeen++;
                        FLIKS_LOG("audio: writing %d samples to audout", converted);
                    }
                    impl.out.write(impl.audioBuffer.data(),
                                   static_cast<size_t>(converted) * AudioOut::kChannels * 2);
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
    }

    FLIKS_LOG("player: session exit, state=%d, dropped=%d, restart=%d",
              static_cast<int>(m_state.load()), m_dropped.load(),
              m_restartPending.load() ? 1 : 0);
    stopPump();
    releasePending();
    av_frame_free(&vframe);
    av_frame_free(&frame);
    av_packet_free(&packet);
    teardown();
}

} // namespace player
