#pragma once

#include <cstddef>
#include <functional>

#include <pthread.h>

namespace util {

// std::thread gives no control over stack size, and libnx's default is small
// next to what ffmpeg's demuxers and decoders want. An overflow here does not
// fault cleanly — it writes into whatever the allocator put next, which on
// this app is often deko3d's pools, and the failure then surfaces as a GPU
// fault far from the cause. So every thread that touches ffmpeg gets an
// explicit, generous stack.
class Thread {
public:
    static constexpr size_t kWorkerStack = 512 * 1024;
    static constexpr size_t kDemuxStack = 1024 * 1024;

    Thread() = default;
    ~Thread();
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    bool start(std::function<void()> fn, size_t stackBytes);
    bool joinable() const { return m_started; }
    void join();

private:
    pthread_t m_handle{};
    bool m_started = false;
};

} // namespace util
