#include "player/Subtitles.h"

#include <algorithm>
#include <cstdlib>

#include "util/Log.h"

namespace player {

namespace {

std::string trimmed(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// "00:01:23.456" or "01:23.456" — WebVTT makes the hours optional.
bool parseTimestamp(const std::string& text, double& out)
{
    int parts[3] = { 0, 0, 0 };
    int count = 0;
    double seconds = 0.0;
    size_t pos = 0;

    while (count < 3 && pos < text.size()) {
        size_t end = pos;
        while (end < text.size() && text[end] != ':') end++;
        const std::string piece = text.substr(pos, end - pos);
        if (piece.empty()) return false;
        if (end >= text.size() || count == 2) {
            seconds = std::atof(piece.c_str());
            parts[count++] = -1;
            break;
        }
        parts[count++] = std::atoi(piece.c_str());
        pos = end + 1;
    }
    if (count == 0) return false;

    // Whatever was read last is the seconds field; the ones before it are
    // minutes, and hours before those.
    double total = seconds;
    if (count >= 2) total += parts[count - 2] * 60.0;
    if (count >= 3) total += parts[count - 3] * 3600.0;
    out = total;
    return true;
}

// Tags are WebVTT's own markup (<i>, <c.yellow>, <00:01.000>), not content.
// Nothing here styles text per-cue, so they are dropped rather than parsed.
std::string stripTags(const std::string& line)
{
    std::string out;
    out.reserve(line.size());
    bool inTag = false;
    for (char c : line) {
        if (c == '<') {
            inTag = true;
            continue;
        }
        if (c == '>') {
            inTag = false;
            continue;
        }
        if (!inTag) out.push_back(c);
    }
    return out;
}

} // namespace

std::string SubtitleTrack::label() const
{
    std::string out = title.empty() ? language : title;
    if (out.empty()) out = "Subtitle";
    if (forced) out += " (forced)";
    if (hearingImpaired) out += " (SDH)";
    return out;
}

void Subtitles::clear()
{
    m_cues.clear();
    m_hint = 0;
}

bool Subtitles::parse(const std::string& vtt)
{
    clear();

    size_t pos = 0;
    int skipped = 0;
    while (pos < vtt.size()) {
        size_t eol = vtt.find('\n', pos);
        if (eol == std::string::npos) eol = vtt.size();
        const std::string line = trimmed(vtt.substr(pos, eol - pos));
        pos = eol + 1;

        // A cue begins at its timing line. Everything before it — the WEBVTT
        // header, NOTE blocks, STYLE blocks, an optional cue id — is skipped
        // by looking for the arrow rather than by tracking what section we
        // are in, which no real file makes easy.
        const size_t arrow = line.find("-->");
        if (arrow == std::string::npos) continue;

        Cue cue;
        const std::string from = trimmed(line.substr(0, arrow));
        std::string to = trimmed(line.substr(arrow + 3));
        // Cue settings (align, line, position) follow the end time.
        const size_t space = to.find(' ');
        if (space != std::string::npos) to = to.substr(0, space);
        if (!parseTimestamp(from, cue.start) || !parseTimestamp(to, cue.end)) {
            skipped++;
            continue;
        }

        while (pos < vtt.size()) {
            size_t textEol = vtt.find('\n', pos);
            if (textEol == std::string::npos) textEol = vtt.size();
            const std::string text = trimmed(vtt.substr(pos, textEol - pos));
            pos = textEol + 1;
            if (text.empty()) break;
            std::string clean = trimmed(stripTags(text));
            if (!clean.empty()) cue.lines.push_back(std::move(clean));
        }
        if (!cue.lines.empty() && cue.end > cue.start) m_cues.push_back(std::move(cue));
    }

    // Files are normally in order already, but a merged or OCR'd one need not
    // be, and at() walks forward on the assumption that they are.
    std::stable_sort(m_cues.begin(), m_cues.end(),
                     [](const Cue& a, const Cue& b) { return a.start < b.start; });

    FLIKS_LOG("subs: %zu cues%s", m_cues.size(),
              skipped > 0 ? " (some timings unreadable)" : "");
    return !m_cues.empty();
}

const Cue* Subtitles::at(double seconds) const
{
    if (m_cues.empty()) return nullptr;

    // A seek can land anywhere, so the hint is only trusted when it is not
    // already past the time asked for.
    if (m_hint >= m_cues.size() || m_cues[m_hint].start > seconds) m_hint = 0;
    while (m_hint + 1 < m_cues.size() && m_cues[m_hint + 1].start <= seconds) m_hint++;

    const Cue& cue = m_cues[m_hint];
    if (seconds >= cue.start && seconds < cue.end) return &cue;
    return nullptr;
}

} // namespace player
