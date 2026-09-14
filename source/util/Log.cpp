#include "util/Log.h"

#include <string>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/stat.h>

namespace util {

namespace {

FILE* g_file = nullptr;
std::mutex g_mutex;

} // namespace

void logInit()
{
    ::mkdir("sdmc:/switch", 0777);
    ::mkdir("sdmc:/switch/fliks", 0777);
    g_file = std::fopen("sdmc:/switch/fliks/log.txt", "w");

    // Written by the Makefile on every build, so a log can always be matched
    // to the binary that produced it.
    char stamp[64] = "unknown";
    if (FILE* f = std::fopen("romfs:/build.txt", "r")) {
        if (std::fgets(stamp, sizeof(stamp), f)) {
            char* nl = std::strchr(stamp, '\n');
            if (nl) *nl = '\0';
        }
        std::fclose(f);
    }
    logf("--- fliks %s (built %s) ---", APP_VERSION, stamp);
}

void logShutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void logf(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;

    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_file, fmt, args);
    va_end(args);
    std::fputc('\n', g_file);
    // Flushed per line: the interesting case is the one where the next thing
    // that happens is a hang or an abort.
    std::fflush(g_file);
}

std::string loggableUrl(const std::string& url)
{
    const size_t q = url.find('?');
    return q == std::string::npos ? url : url.substr(0, q);
}

} // namespace util
