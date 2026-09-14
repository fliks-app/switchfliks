#pragma once

#include <string>

// deko3d owns the framebuffer, so there is no console to print to and a
// failure during bring-up shows as a black screen and nothing else. Every
// interesting step therefore goes to sdmc:/switch/fliks/log.txt.
namespace util {

// Stream URLs carry a JWT in the query. Logs get attached to bug reports, so
// nothing that grants access to a library may reach one: the query is cut off
// at the '?' and only the path survives.
std::string loggableUrl(const std::string& url);

void logInit();
void logShutdown();
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace util

#define FLIKS_LOG(...) ::util::logf(__VA_ARGS__)
