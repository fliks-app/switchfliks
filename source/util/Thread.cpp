#include "util/Thread.h"

#include <memory>

#include "util/Log.h"

namespace util {

namespace {

void* trampoline(void* arg)
{
    std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()>*>(arg));
    (*fn)();
    return nullptr;
}

} // namespace

Thread::~Thread() { join(); }

bool Thread::start(std::function<void()> fn, size_t stackBytes)
{
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return false;
    pthread_attr_setstacksize(&attr, stackBytes);

    auto* payload = new std::function<void()>(std::move(fn));
    const int rc = pthread_create(&m_handle, &attr, trampoline, payload);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        FLIKS_LOG("thread: pthread_create failed (%d) for a %zu KiB stack", rc, stackBytes / 1024);
        delete payload;
        return false;
    }
    m_started = true;
    return true;
}

void Thread::join()
{
    if (!m_started) return;
    pthread_join(m_handle, nullptr);
    m_started = false;
}

} // namespace util
