#pragma once

// Optional overrides read from sdmc:/switch/fliks/config.txt. The file need
// not exist — every key has a working default — but it lets a playback
// problem be narrowed on-device without a rebuild. One `key=value` per line,
// `#` comments ignored; every effective value is echoed to the log.
//
//   hwdec=1           0 falls back to software decoding instead of NVDEC
//   decodeThreads=3   ffmpeg thread_count, software path only
//   skipLoopFilter=0  1 drops deblocking, ~20% of the decode budget back
//   videoQueue=24     decoded frames held ahead, capped to 24 MiB of frames
//   maxHeight=720     forces a rung by height, overriding the quality menu
//   maxBitrate=12000000  what the server is told this device can pull, in
//                     bits per second. Lower it below the real link speed and
//                     the server transcodes instead of direct-playing a file
//                     the connection cannot carry.
//   trace=0           1 logs per-frame playback state (costly: SD writes)
namespace util {

void loadConfig();
int configInt(const char* key, int fallback);

} // namespace util
