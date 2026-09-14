#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gfx {

struct DecodedImage {
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;

    bool valid() const { return width > 0 && height > 0 && !rgba.empty(); }
};

// JPEG, PNG and WebP all come back through libavcodec rather than three
// separate decoders: ffmpeg is already linked for playback, and its image
// demuxers cover everything Fliks serves for artwork.
//
// Thread-safe; called from the task-queue workers. `maxWidth` downscales
// during conversion so a 1920px fanart never lands in VRAM at full size.
bool decodeImage(const uint8_t* data, size_t size, int maxWidth, DecodedImage& out);

inline bool decodeImage(const std::string& bytes, int maxWidth, DecodedImage& out)
{
    return decodeImage(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), maxWidth, out);
}

} // namespace gfx
