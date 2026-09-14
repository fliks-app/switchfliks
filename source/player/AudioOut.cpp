#include "player/AudioOut.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#include <switch.h>

#include "util/Log.h"

namespace player {

namespace {

// 42 ms per buffer: coarse enough to be cheap, fine enough that the audio
// clock the video syncs against never drifts visibly.
constexpr int kFramesPerBuffer = 2048;
constexpr int kBytesPerFrame = AudioOut::kChannels * 2;
constexpr int kBufferBytes = kFramesPerBuffer * kBytesPerFrame;
constexpr int kBufferCount = 6;
// 5.5 seconds of audio. The decoder fills it whenever the network lets
// it, and that reserve is what carries playback across a stall.
constexpr size_t kRingBytes = 1024 * 1024;
// Five buffers stay queued behind the one being refilled, so waiting a few
// milliseconds for a complete one never risks the driver running dry.
constexpr int kFillRetries = 12;
constexpr uint64_t kFillWaitNs = 3'000'000ULL;

size_t alignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

} // namespace

bool AudioOut::start()
{
    if (m_running.load()) return true;
    const Result initRc = audoutInitialize();
    if (R_FAILED(initRc)) {
        FLIKS_LOG("audio: audoutInitialize failed 0x%x", initRc);
        return false;
    }
    const Result startRc = audoutStartAudioOut();
    if (R_FAILED(startRc)) {
        FLIKS_LOG("audio: audoutStartAudioOut failed 0x%x", startRc);
        audoutExit();
        return false;
    }

    m_ring.assign(kRingBytes, 0);
    m_head = m_tail = m_filled = 0;
    m_writtenBytes = 0;
    m_playedFrames = 0;
    m_running = true;
    m_thread = std::thread([this] { threadMain(); });
    return true;
}

void AudioOut::stop()
{
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();
    audoutStopAudioOut();
    audoutExit();
    // Only now: until the service has stopped it can still be holding the
    // buffers the thread handed it.
    free(m_pool);
    m_pool = nullptr;
    if (m_underruns > 0) FLIKS_LOG("audio: %d underruns", m_underruns.load());
    m_underruns = 0;
}

void AudioOut::write(const uint8_t* data, size_t bytes)
{
    size_t written = 0;
    while (written < bytes && m_running.load()) {
        size_t chunk = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const size_t space = m_ring.size() - m_filled;
            chunk = std::min(bytes - written, space);
            for (size_t i = 0; i < chunk; i++) {
                m_ring[m_tail] = data[written + i];
                m_tail = (m_tail + 1) % m_ring.size();
            }
            m_filled += chunk;
        }
        written += chunk;
        m_writtenBytes.fetch_add(static_cast<int64_t>(chunk));
        // Full ring: the decoder is ahead of playback, which is exactly when
        // it should stop running.
        if (chunk == 0) svcSleepThread(4'000'000ULL);
    }
}

void AudioOut::setPaused(bool paused) { m_paused = paused; }

void AudioOut::flush()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_head = m_tail = m_filled = 0;
    m_playedFrames = 0;
}

double AudioOut::clock() const
{
    return static_cast<double>(m_playedFrames.load()) / kSampleRate;
}

size_t AudioOut::queuedBytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_filled;
}

void AudioOut::threadMain()
{
    // audout wants its buffers page-aligned and their size a multiple of
    // 0x1000, so the pool is one allocation carved into slots.
    const size_t slotBytes = alignUp(kBufferBytes, 0x1000);
    m_pool = static_cast<uint8_t*>(memalign(0x1000, slotBytes * kBufferCount));
    if (!m_pool) return;
    uint8_t* pool = m_pool;
    std::memset(pool, 0, slotBytes * kBufferCount);

    std::vector<AudioOutBuffer> buffers(kBufferCount);
    // How much of each slot is real audio rather than silence padding, so the
    // clock counts only what was actually decoded.
    std::vector<size_t> realFrames(kBufferCount, 0);
    for (int i = 0; i < kBufferCount; i++) {
        buffers[i] = AudioOutBuffer{};
        buffers[i].next = nullptr;
        buffers[i].buffer = pool + static_cast<size_t>(i) * slotBytes;
        buffers[i].buffer_size = slotBytes;
        buffers[i].data_size = kBufferBytes;
        buffers[i].data_offset = 0;
        audoutAppendAudioOutBuffer(&buffers[i]);
    }

    while (m_running.load()) {
        AudioOutBuffer* released = nullptr;
        u32 count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100'000'000ULL)) || !released)
            continue;

        int slot = 0;
        for (int i = 0; i < kBufferCount; i++)
            if (&buffers[i] == released) slot = i;
        m_playedFrames.fetch_add(static_cast<int64_t>(realFrames[slot]));

        auto* dst = static_cast<uint8_t*>(released->buffer);
        size_t copied = 0;
        if (!m_paused.load()) {
            // Submitting a half-full buffer splices silence into the middle
            // of the waveform, which is heard as a click — and under load
            // that happens often enough to sound like constant crackle.
            // There are five more buffers queued behind this one, so a short
            // wait for a whole one costs nothing and avoids the splice.
            for (int spin = 0; spin < kFillRetries; spin++) {
                {
                    // The sleep must happen outside the lock: waiting for the
                    // ring to fill while holding the mutex blocks the very
                    // thread that fills it.
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_filled >= static_cast<size_t>(kBufferBytes)) break;
                    if (!m_running.load()) break;
                }
                svcSleepThread(kFillWaitNs);
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            copied = std::min(static_cast<size_t>(kBufferBytes), m_filled);
            for (size_t i = 0; i < copied; i++) {
                dst[i] = m_ring[m_head];
                m_head = (m_head + 1) % m_ring.size();
            }
            m_filled -= copied;
            if (copied < static_cast<size_t>(kBufferBytes)) m_underruns++;
        }
        // Underrun or paused: silence keeps the buffer chain alive so the
        // driver does not stall and the clock keeps a steady cadence.
        if (copied < static_cast<size_t>(kBufferBytes))
            std::memset(dst + copied, 0, kBufferBytes - copied);
        realFrames[slot] = copied / kBytesPerFrame;

        released->data_size = kBufferBytes;
        released->data_offset = 0;
        audoutAppendAudioOutBuffer(released);
    }
}

} // namespace player
