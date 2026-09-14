#pragma once

#include <string>
#include <vector>

namespace player {

// One selectable subtitle track. `streamIndex` addresses an embedded stream
// inside the container and `subtitleId` a row the server stores beside the
// file; exactly one of them is set, which is what decides the URL.
struct SubtitleTrack {
    int streamIndex = -1;
    int subtitleId = -1;
    std::string language;
    std::string title;
    std::string codec;
    bool forced = false;
    bool hearingImpaired = false;
    std::string label() const;
};

struct Cue {
    double start = 0;
    double end = 0;
    // Already split on the line breaks the file carried.
    std::vector<std::string> lines;
};

// The server renders every subtitle — embedded, external, OCR'd — to WebVTT,
// so this needs to read one format and nothing else. A film's worth of cues
// is a few hundred kilobytes, well worth fetching once rather than streaming
// alongside the video and having to keep two positions in step.
//
// Bitmap subtitles (PGS, VOBSUB) are not here: they are pictures, and the
// server burns those into the video during a transcode instead.
class Subtitles {
public:
    // Parses a WebVTT body. Cues arrive sorted by start time; malformed ones
    // are skipped rather than failing the file, since one bad timestamp
    // should not cost the other two thousand cues.
    bool parse(const std::string& vtt);
    void clear();

    bool empty() const { return m_cues.empty(); }
    size_t size() const { return m_cues.size(); }

    // The cue covering `seconds`, or null in a gap. The search resumes from
    // the last hit, so playing forward costs one comparison a frame.
    const Cue* at(double seconds) const;

private:
    std::vector<Cue> m_cues;
    mutable size_t m_hint = 0;
};

} // namespace player
