#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "util/Thread.h"

struct AVFrame;

namespace player {

struct Settings {
    // A ladder rung id as the server names them — "720p", "eco-720p",
    // "original" — rather than a height. Two rungs can share a height and
    // differ only in bitrate, and on a link that cannot carry the full rung
    // that is precisely the choice worth having.
    std::string quality = "720p";
    bool showStats = false;
};

// The height a rung id implies, for capping the HLS variant: 720 for both
// "720p" and "eco-720p", 0 for "auto" and "original" (no cap).
int qualityHeight(const std::string& id);
// Rough bitrate ceiling a rung id implies, or 0 when unknown. Needed because
// a master playlist lists eco and full rungs at the same height, and height
// alone cannot tell them apart.
int64_t qualityBitrate(const std::string& id);

Settings settings();
// Writes sdmc:/switch/fliks/settings.json as well; a rung the viewer picked
// should still be picked next launch.
void setSettings(const Settings& s);
// Reads that file. Absent or unparseable leaves the defaults in place.
void loadSettings();

enum class State { Idle, Opening, Playing, Paused, Buffering, Ended, Failed };

// One selectable audio stream inside the container. The index is ffmpeg's
// stream index, which is what the demuxer needs and what survives a session
// restart — a fed stream rebuilds its format context on every seek.
struct AudioTrack {
    int index = 0;
    std::string language;
    std::string title;
    std::string codec;
    int channels = 0;
    // "English · AC-3 5.1"
    std::string label() const;
};

class AudioOut;

class Player {
public:
    Player();
    ~Player();

    bool open(const std::string& url, double startAtSeconds);
    void close();

    void setPaused(bool paused);
    bool paused() const { return m_paused.load(); }
    void togglePause() { setPaused(!paused()); }
    // The main thread drives the fallback clock; it is what keeps video
    // moving on a stream with no audio, or when audout refuses to start.
    void advanceClock(double dt);

    void seekTo(double seconds);
    void seekBy(double delta);

    State state() const { return m_state.load(); }
    std::string error() const;
    double position() const;
    double duration() const { return m_duration.load(); }
    int videoWidth() const { return m_videoWidth.load(); }
    int videoHeight() const { return m_videoHeight.load(); }

    // Main thread. Hands over the frame whose presentation time has arrived,
    // dropping anything already late. The caller must release it.
    AVFrame* acquireFrame();
    void releaseFrame(AVFrame* frame);

    // Snapshot; the demuxer fills this once a container is open.
    std::vector<AudioTrack> audioTracks() const;
    int currentAudioTrack() const { return m_audioTrack.load(); }
    // Applied by the demux thread at a packet boundary: switching means
    // tearing down one decoder and building another, which cannot happen
    // underneath a decode in flight.
    void selectAudioTrack(int streamIndex);

    int droppedFrames() const { return m_dropped.load(); }
    int queuedFrames() const;
    double bufferedSeconds() const;

private:
    void demuxLoop();
    // One open/decode/teardown cycle; a fed stream restarts by running
    // another, since its bytes are a concatenation with no index to seek.
    void runSession(double startAt);
    static int interruptCb(void* opaque);
    void fail(const std::string& message);
    void clearFrames();

    struct Impl;
    Impl* m_impl = nullptr;

    util::Thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<AVFrame*> m_frames;

    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_paused{ false };
    std::atomic<State> m_state{ State::Idle };
    std::atomic<double> m_duration{ 0.0 };
    // Stream PTS that corresponds to zero on whichever clock is driving.
    std::atomic<double> m_clockBase{ 0.0 };
    std::atomic<bool> m_clockAnchored{ false };
    std::atomic<bool> m_useAudioClock{ false };
    // Wall-clock fallback keeps its own base, so dropping to it and
    // returning never disturbs the audio anchor in m_clockBase.
    std::atomic<double> m_wallBase{ 0.0 };
    std::atomic<double> m_fallbackClock{ 0.0 };
    std::atomic<bool> m_audioAvailable{ false };
    std::atomic<double> m_lastAudioClock{ 0.0 };
    std::atomic<double> m_audioStallSeconds{ 0.0 };
    std::atomic<double> m_lastPts{ 0.0 };
    std::atomic<double> m_seekRequest{ -1.0 };
    std::atomic<int> m_audioTrack{ -1 };
    std::atomic<int> m_audioTrackRequest{ -1 };
    std::vector<AudioTrack> m_audioTracks;
    // Resume offset, applied as a normal seek once frames are flowing
    // rather than against a stream that has never produced one.
    std::atomic<double> m_pendingStartSeek{ -1.0 };
    std::atomic<bool> m_restartPending{ false };
    std::atomic<double> m_restartAt{ 0.0 };
    std::atomic<int> m_videoWidth{ 0 };
    std::atomic<int> m_videoHeight{ 0 };
    std::atomic<int> m_dropped{ 0 };

    mutable std::mutex m_errorMutex;
    std::string m_error;
    std::string m_openUrl;   // written before the thread starts, read by it
};

} // namespace player
