#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace player {

// audout is fixed at 48 kHz stereo PCM16, so the decoder resamples to that
// and this only shuttles bytes. The count of samples the driver has actually
// retired is the master clock the video syncs to.
class AudioOut {
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 2;

    bool start();
    void stop();

    // Called from the decode thread. Blocks briefly when the ring is full,
    // which is what paces decoding to real time.
    void write(const uint8_t* data, size_t bytes);

    void setPaused(bool paused);
    void flush();

    // Seconds of audio the driver has played since the last flush.
    double clock() const;
    size_t queuedBytes() const;
    // Total PCM handed over since start; the rate it grows at says whether
    // the decoder is keeping the device fed.
    int64_t writtenBytes() const { return m_writtenBytes.load(); }
    int underruns() const { return m_underruns.load(); }

private:
    void threadMain();

    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_paused{ false };
    std::atomic<int64_t> m_playedFrames{ 0 };
    std::atomic<int> m_underruns{ 0 };
    std::atomic<int64_t> m_writtenBytes{ 0 };

    uint8_t* m_pool = nullptr;

    mutable std::mutex m_mutex;
    std::vector<uint8_t> m_ring;
    size_t m_head = 0;
    size_t m_tail = 0;
    size_t m_filled = 0;
};

} // namespace player
