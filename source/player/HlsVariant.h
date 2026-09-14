#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace player {

// One `#EXT-X-MEDIA:TYPE=AUDIO` rendition from a master playlist. The server
// splits audio out of the variant whenever a source carries more than one
// track, so on those titles this list is the only place audio exists at all.
struct HlsAudioRendition {
    std::string url;   // absolute, already joined against the master
    std::string name;
    std::string language;
    int channels = 0;
    bool isDefault = false;
};

// ffmpeg's HLS demuxer picks a variant by itself and takes the first one in
// the master playlist, which is the highest rung the server published. On
// this hardware that means trying to software-decode 4K. The server's own
// ladder is advisory only — the device profile caps what it *transcodes*,
// not what it *lists* — so the rung is chosen here and the variant playlist
// is handed to ffmpeg directly, never the master.
//
// Returns false when the body is not a master playlist (a media playlist, or
// anything unparseable), in which case the caller should use the URL as-is.
// `maxHeight` of 0 means no cap. `maxBandwidth` disambiguates rungs that share
// a height: the server publishes an eco 720p at 1.63 Mbit/s beside a full one
// at 4.13, and picking on height alone always takes the expensive one — which
// is wrong on exactly the links where the choice matters. 0 means no
// preference, i.e. the tallest, richest rung that fits the height cap.
//
// `audioOut`, when given, receives the renditions in the chosen variant's
// `AUDIO` group, in playlist order. Empty means the variant carries its own
// audio muxed in, which is what the server does for single-track sources.
bool selectHlsVariant(const std::string& masterUrl, const std::string& masterBody, int maxHeight,
                      int64_t maxBandwidth, std::string& variantUrl, int& chosenHeight,
                      std::vector<HlsAudioRendition>* audioOut = nullptr);

} // namespace player
