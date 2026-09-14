#include "util/TextInput.h"

#include <vector>

#include <switch.h>

namespace util {

bool textInput(const char* guide, const std::string& initial, std::string& out, bool password,
               size_t maxLength)
{
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;

    if (password) swkbdConfigMakePresetPassword(&kbd);
    else swkbdConfigMakePresetDefault(&kbd);

    swkbdConfigSetGuideText(&kbd, guide);
    swkbdConfigSetHeaderText(&kbd, guide);
    if (!initial.empty()) swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, static_cast<u32>(maxLength));
    swkbdConfigSetBlurBackground(&kbd, 1);

    std::vector<char> buffer(maxLength + 1, 0);
    const Result rc = swkbdShow(&kbd, buffer.data(), buffer.size());
    swkbdClose(&kbd);

    if (R_FAILED(rc)) return false;
    out.assign(buffer.data());
    return true;
}

} // namespace util
