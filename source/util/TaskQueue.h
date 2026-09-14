#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "util/Thread.h"

namespace util {

// Background work is HTTP and image decode, both of which block for tens of
// milliseconds. Keeping them off the render thread is what lets the rails
// stay at 60fps while artwork streams in.
class TaskQueue {
public:
    void start(int threads);
    // Workers finish whatever is already queued before they exit, so the
    // progress report a screen posts on its way out still goes.
    void stop();

    // Lower priority runs after everything queued at higher priority, so a
    // detail-page fetch is not stuck behind forty poster downloads.
    enum class Priority { High, Low };
    void post(std::function<void()> job, Priority priority = Priority::Low);

    // Callable from a worker; runs on the next drainMain().
    void postToMain(std::function<void()> fn);
    void drainMain();

    // Bumped whenever the user navigates; jobs captured with an older token
    // drop their result instead of mutating a screen that is gone.
    uint64_t generation() const { return m_generation.load(); }
    uint64_t bumpGeneration() { return ++m_generation; }

private:
    void workerLoop();

    std::vector<std::unique_ptr<Thread>> m_threads;
    std::deque<std::function<void()>> m_high;
    std::deque<std::function<void()>> m_low;
    std::mutex m_jobMutex;
    std::condition_variable m_jobCv;

    std::vector<std::function<void()>> m_mainQueue;
    std::mutex m_mainMutex;

    std::atomic<bool> m_running{ false };
    std::atomic<uint64_t> m_generation{ 1 };
};

} // namespace util
