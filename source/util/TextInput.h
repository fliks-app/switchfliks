#pragma once

#include <string>

namespace util {

// The system software keyboard. Blocking — the applet takes over the screen,
// and libnx resumes the render loop when it returns.
bool textInput(const char* guide, const std::string& initial, std::string& out,
               bool password = false, size_t maxLength = 255);

} // namespace util
