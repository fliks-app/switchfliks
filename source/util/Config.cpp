#include "util/Config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "util/Log.h"

namespace util {

namespace {

std::map<std::string, int> g_values;

std::string trimmed(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // namespace

void loadConfig()
{
    FILE* f = std::fopen("sdmc:/switch/fliks/config.txt", "r");
    if (!f) {
        FLIKS_LOG("config: none, using defaults");
        return;
    }

    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        std::string text = trimmed(line);
        if (text.empty() || text[0] == '#') continue;
        const size_t eq = text.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trimmed(text.substr(0, eq));
        const int value = std::atoi(trimmed(text.substr(eq + 1)).c_str());
        if (key.empty()) continue;
        g_values[key] = value;
        FLIKS_LOG("config: %s=%d", key.c_str(), value);
    }
    std::fclose(f);
}

int configInt(const char* key, int fallback)
{
    auto it = g_values.find(key);
    return it == g_values.end() ? fallback : it->second;
}

} // namespace util
