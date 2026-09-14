#include "util/TaskQueue.h"

namespace util {

void TaskQueue::start(int threads)
{
    if (m_running.exchange(true)) return;
    for (int i = 0; i < threads; i++) {
        auto worker = std::unique_ptr<Thread>(new Thread());
        // Image decode runs libavcodec on these.
        if (worker->start([this] { workerLoop(); }, Thread::kWorkerStack))
            m_threads.push_back(std::move(worker));
    }
}

void TaskQueue::stop()
{
    if (!m_running.exchange(false)) return;
    m_jobCv.notify_all();
    for (auto& t : m_threads) t->join();
    m_threads.clear();

    std::lock_guard<std::mutex> lock(m_jobMutex);
    m_high.clear();
    m_low.clear();
}

void TaskQueue::post(std::function<void()> job, Priority priority)
{
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        if (priority == Priority::High) m_high.push_back(std::move(job));
        else m_low.push_back(std::move(job));
    }
    m_jobCv.notify_one();
}

void TaskQueue::postToMain(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lock(m_mainMutex);
    m_mainQueue.push_back(std::move(fn));
}

void TaskQueue::drainMain()
{
    std::vector<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lock(m_mainMutex);
        batch.swap(m_mainQueue);
    }
    for (auto& fn : batch) fn();
}

void TaskQueue::workerLoop()
{
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(m_jobMutex);
            m_jobCv.wait(lock, [this] {
                return !m_running.load() || !m_high.empty() || !m_low.empty();
            });
            if (!m_running.load() && m_high.empty() && m_low.empty()) return;
            if (!m_high.empty()) {
                job = std::move(m_high.front());
                m_high.pop_front();
            } else if (!m_low.empty()) {
                job = std::move(m_low.front());
                m_low.pop_front();
            }
        }
        if (job) job();
    }
}

} // namespace util
