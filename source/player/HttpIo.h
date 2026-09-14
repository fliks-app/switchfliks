#pragma once

#include <string>

struct AVFormatContext;
struct AVIOContext;
struct AVDictionary;

namespace player {


// devkitPro's ffmpeg is built without a TLS backend, so its https protocol is
// a stub and an `https://` manifest never opens. Every byte therefore comes
// through the app's own mbedtls client instead: the master playlist over a
// custom AVIOContext, and the variant playlists and segments the HLS demuxer
// reaches for through the io_open hook below.
//
// It doubles as the place the stream token rides along, and as the hook that
// makes an abort actually tear the socket down instead of waiting on a read.

// Installs the custom IO on a freshly allocated (not yet opened) context.
// Returns false if the first request fails.
bool attachHttpIo(AVFormatContext* fmt, const std::string& url);

// Same, but fed by a walked HLS playlist rather than one URL: ffmpeg sees a
// single continuous fMP4 stream and its HLS demuxer is never involved.
class HlsFeed;
bool attachFeedIo(AVFormatContext* fmt, HlsFeed* feed);

// Frees the AVIOContext attachHttpIo installed. Call after
// avformat_close_input, which does not own custom IO.
void detachHttpIo(AVIOContext* pb);
void detachFeedIo(AVIOContext* pb);

// Flips every open stream of this context into an aborted state so a blocked
// read returns instead of holding the decode thread at teardown.
void abortHttpIo(AVFormatContext* fmt);

} // namespace player
