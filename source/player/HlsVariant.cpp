#include "player/HlsVariant.h"

#include <cstdlib>
#include <vector>

#include "net/Http.h"
#include "util/Log.h"

namespace player {

namespace {

struct Variant {
    int height = 0;
    long long bandwidth = 0;
    std::string uri;
};

std::string trimmed(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// RESOLUTION=3840x1608 -> 1608
int heightOf(const std::string& line)
{
    const size_t at = line.find("RESOLUTION=");
    if (at == std::string::npos) return 0;
    const size_t x = line.find('x', at);
    if (x == std::string::npos) return 0;
    return std::atoi(line.c_str() + x + 1);
}

long long bandwidthOf(const std::string& line)
{
    // AVERAGE-BANDWIDTH also ends in "BANDWIDTH=", so anchor on the first
    // occurrence that starts the attribute.
    size_t at = line.find("BANDWIDTH=");
    if (at == std::string::npos) return 0;
    if (at > 0 && line[at - 1] == '-') {
        at = line.find("BANDWIDTH=", at + 1);
        if (at == std::string::npos) return 0;
    }
    return std::atoll(line.c_str() + at + 10);
}

// How far apart two bitrates are in scale rather than in absolute terms:
// 1.0 is identical, 2.0 is double or half. Symmetrical, so neither side of
// the comparison is privileged.
double bitrateDistance(int64_t bandwidth, int64_t wanted)
{
    if (bandwidth <= 0 || wanted <= 0) return 1e9;
    const double a = static_cast<double>(bandwidth);
    const double b = static_cast<double>(wanted);
    return a > b ? a / b : b / a;
}

} // namespace

bool selectHlsVariant(const std::string& masterUrl, const std::string& masterBody, int maxHeight,
                      int64_t maxBandwidth, std::string& variantUrl, int& chosenHeight)
{
    if (masterBody.find("#EXT-X-STREAM-INF") == std::string::npos) return false;

    std::vector<Variant> variants;
    size_t pos = 0;
    while (pos < masterBody.size()) {
        size_t eol = masterBody.find('\n', pos);
        if (eol == std::string::npos) eol = masterBody.size();
        const std::string line = trimmed(masterBody.substr(pos, eol - pos));
        pos = eol + 1;

        if (line.rfind("#EXT-X-STREAM-INF", 0) != 0) continue;

        // The URI is the next non-blank, non-comment line.
        while (pos < masterBody.size()) {
            size_t uriEol = masterBody.find('\n', pos);
            if (uriEol == std::string::npos) uriEol = masterBody.size();
            const std::string uri = trimmed(masterBody.substr(pos, uriEol - pos));
            pos = uriEol + 1;
            if (uri.empty() || uri[0] == '#') continue;
            variants.push_back(Variant{ heightOf(line), bandwidthOf(line), uri });
            break;
        }
    }
    if (variants.empty()) return false;

    // Height decides the rung; bandwidth only separates variants that share
    // one. Treating the requested bitrate as a ceiling dropped a level every
    // time — the master's measured BANDWIDTH runs about 1.5x the ladder's
    // nominal figure (1080p is published at 8.19 Mbit/s and measures 12.29),
    // so asking for 1080p landed on 720p and asking for 720p landed on 480p.
    int bestHeight = -1;
    for (const Variant& v : variants) {
        if (maxHeight > 0 && v.height > 0 && v.height > maxHeight) continue;
        if (v.height > bestHeight) bestHeight = v.height;
    }

    const Variant* best = nullptr;
    if (bestHeight >= 0) {
        for (const Variant& v : variants) {
            if (v.height != bestHeight) continue;
            if (!best) {
                best = &v;
                continue;
            }
            // Compared as a ratio, not a difference: the nominal-to-measured
            // gap is proportional, so the eco rung at the same height is the
            // one whose bandwidth is nearest in scale, whatever the offset.
            if (maxBandwidth > 0 && v.bandwidth > 0 && best->bandwidth > 0) {
                if (bitrateDistance(v.bandwidth, maxBandwidth) <
                    bitrateDistance(best->bandwidth, maxBandwidth))
                    best = &v;
            } else if (v.bandwidth > best->bandwidth) {
                best = &v;
            }
        }
    }

    // Every rung is above the cap: the shortest is the only one with a chance.
    if (!best) {
        for (const Variant& v : variants)
            if (!best || (v.height > 0 && v.height < best->height)) best = &v;
    }
    if (!best) return false;

    variantUrl = net::joinUrl(masterUrl, best->uri);
    chosenHeight = best->height;
    FLIKS_LOG("hls: %zu variants, picked %dp at %.2f Mbit/s (cap %dp, %.2f Mbit/s)",
              variants.size(), best->height, best->bandwidth / 1.0e6, maxHeight,
              maxBandwidth / 1.0e6);
    return true;
}

} // namespace player
