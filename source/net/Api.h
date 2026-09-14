#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "net/Http.h"
#include "player/Subtitles.h"

namespace api {

struct User {
    int id = 0;
    std::string username;
    std::string avatar;
    bool isAdmin = false;
};

struct Library {
    int id = 0;
    std::string name;
    std::string icon;
    std::string color;
    bool hasSeries = false;
    bool hasMovies = false;
};

// One entry in a home rail or a library grid. Mirrors what `app-media-card`
// binds, not the full `Media` the detail page loads.
struct MediaCard {
    int id = 0;
    std::string title;
    std::string subtitle;
    int year = 0;
    bool series = false;
    std::string posterUrl;
    std::string fanartUrl;
    double rating = 0;
    bool watched = false;
    float progressPercent = 0;
    bool available = true;

    // Continue-watching only.
    int mediaFileId = 0;
    int episodeId = 0;
    double positionSeconds = 0;
    bool landscape = false;
};

struct Episode {
    int id = 0;
    int number = 0;
    std::string title;
    std::string overview;
    std::string airDate;
    std::string stillUrl;
    int runtime = 0;
    bool hasFile = false;
    bool watched = false;
    float progressPercent = 0;
};

struct Season {
    int id = 0;
    int number = 0;
    std::string posterUrl;
    std::vector<Episode> episodes;
};

// Everything the file-information panel shows. All of it is stored on the
// media file — `relativePath`, `size`, `quality`, `createdAt` are its own
// columns and the rest is the `streamInfo` the scanner probed once — so it
// arrives with the media detail and nothing has to be worked out here.
struct VideoStreamInfo {
    std::string codec;
    std::string profile;
    std::string pixelFormat;
    std::string frameRate;
    std::string colorSpace;
    std::string hdrFormat;
    std::string aspectRatio;
    int width = 0;
    int height = 0;
    int bitDepth = 0;
    int64_t bitrateBps = 0;
};

struct AudioStreamInfo {
    std::string codec;
    std::string language;
    std::string title;
    std::string channelLayout;
    int channels = 0;
    int sampleRate = 0;
    int64_t bitrateBps = 0;
};

struct MediaFile {
    int id = 0;
    std::string quality;
    int64_t size = 0;
    int episodeId = 0;
    std::string path;
    std::string addedAt;
    int64_t bitrateBps = 0;
    bool hasStreamInfo = false;
    VideoStreamInfo video;
    std::vector<AudioStreamInfo> audio;
    // Text subtitle tracks only. Bitmap ones (PGS, VOBSUB) are pictures the
    // server burns into a transcode; there is nothing this client can draw.
    std::vector<player::SubtitleTrack> subtitles;
};

struct CastEntry {
    int personId = 0;
    std::string name;
    std::string character;
    std::string avatarUrl;
};

// `GET /api/persons/:id` -> { person, cast, crew }. The credits are the
// person's roles in *this* library, not their whole filmography, which is
// what makes the page worth having: it is a way into what you can watch.
struct PersonCredit {
    MediaCard media;
    std::string role;   // character for cast, job for crew
};

struct PersonDetail {
    int id = 0;
    std::string name;
    std::string avatarUrl;
    std::string biography;
    std::string birthday;
    std::string deathday;
    std::string placeOfBirth;
    std::string knownForDepartment;
    std::vector<PersonCredit> cast;
    std::vector<PersonCredit> crew;
};

struct MediaDetail {
    int id = 0;
    std::string title;
    std::string overview;
    std::string posterUrl;
    std::string fanartUrl;
    std::string logoUrl;
    std::string libraryName;
    std::string status;
    int year = 0;
    int runtime = 0;
    double rating = 0;
    bool series = false;
    bool watched = false;
    float progressPercent = 0;
    std::vector<std::string> genres;
    std::vector<Season> seasons;
    std::vector<MediaFile> files;
    std::vector<CastEntry> cast;

    bool liked = false;

    // Resume state resolved alongside the detail fetch.
    int resumeFileId = 0;
    int resumeEpisodeId = 0;
    double resumePositionSeconds = 0;
};

struct QualityRung {
    std::string id;
    std::string label;
    int height = 0;
    int64_t bitrateBps = 0;
    bool isRemux = false;
};

// A rung's name for a person: the id distinguishes rungs a label cannot,
// because the eco rungs share "1080p"/"720p" with the full-bitrate ones and
// differ only in bitrate — which is the whole point of choosing between them.
std::string qualityName(const QualityRung& rung);
// "1080p eco · 3.19 Mbit/s"
std::string qualityDescription(const QualityRung& rung);

// The ladder the server last reported, so the Settings screen can offer what
// this server actually serves instead of a hardcoded 720/1080/480 cycle. It
// is only known after a playback-info call, so it starts empty.
std::vector<QualityRung> knownQualities();
void setKnownQualities(std::vector<QualityRung> rungs);

struct PlaybackInfo {
    int mediaFileId = 0;
    std::string playMethod;
    std::string playUrl;      // absolute, token-carrying
    std::string sessionId;
    std::string container;
    double durationSeconds = 0;
    int width = 0;
    int height = 0;
    std::vector<QualityRung> qualities;
    std::string quality;
};

struct SearchParams {
    std::string q;
    int libraryId = 0;
    int page = 1;
    int limit = 60;
    std::string sortBy = "title";
    std::string sortOrder = "ASC";
    const char* type = nullptr;   // "movie" | "series"
};

// One instance, shared by the UI thread and the worker pool; every public
// method is safe to call concurrently.
class Client {
public:
    void setServer(const std::string& baseUrl);
    std::string server() const;
    bool hasSession() const;
    User currentUser() const;

    bool login(const std::string& username, const std::string& password, std::string& error);
    bool resumeSession(std::string& error);
    void logout();

    // Persisted under sdmc:/switch/fliks/session.json.
    bool loadSession();
    void saveSession() const;

    bool fetchLibraries(std::vector<Library>& out);
    bool fetchContinueWatching(std::vector<MediaCard>& out);
    bool fetchRecommendations(std::vector<MediaCard>& out);
    bool fetchRecentlyAdded(int libraryId, int limit, std::vector<MediaCard>& out);
    bool fetchMediaPage(const SearchParams& params, std::vector<MediaCard>& out, int& total);
    bool fetchMediaDetail(int id, MediaDetail& out);
    bool fetchPerson(int id, PersonDetail& out);

    // Flips the server's watched flag for a media (and optionally one episode)
    // and reports back what it settled on, since the server decides — marking
    // a part-watched film watched also clears its resume point.
    bool toggleWatched(int mediaId, int mediaFileId, int episodeId, bool& watchedOut);
    // The heart on the detail page. `state` is read once with the detail.
    bool fetchLiked(int mediaId, bool& likedOut);
    bool setLiked(int mediaId, bool liked);
    // The WebVTT the server renders for one track, whatever its source was.
    std::string subtitleUrl(int mediaFileId, const player::SubtitleTrack& track);
    bool fetchPlaybackInfo(int mediaFileId, int startAtSeconds, const std::string& quality,
                           PlaybackInfo& out, std::string& error);
    bool reportProgress(int mediaId, int mediaFileId, int episodeId, double positionSeconds,
                        double durationSeconds, const std::string& sessionId,
                        const std::string& state);
    void stopSession(const std::string& sessionId);

    // Absolute URL for an artwork path, with the server's pre-generated
    // variant appended the way `resolveUrl | size` does.
    std::string imageUrl(const std::string& url, const char* size) const;
    // Raw bytes for an image; used by the texture loader on worker threads.
    bool fetchBytes(const std::string& absoluteUrl, std::string& out);

private:
    net::Response call(const std::string& method, const std::string& path,
                       const std::string& body = {}, bool retryOn401 = true);
    bool refreshTokens();
    bool ensureStreamToken();
    std::string streamTokenLocked();

    mutable std::mutex m_mutex;
    std::string m_server;
    std::string m_accessToken;
    std::string m_refreshToken;
    std::string m_streamToken;
    int64_t m_streamTokenExpiresAtMs = 0;
    User m_user;
};

// The profile the backend uses to pick a ladder. Software decode on the
// Tegra X1 is the binding constraint, so it advertises h264 up to High@4.1
// and 8-bit only, which keeps the server off HEVC and 10-bit entirely.
std::string switchDeviceProfileJson();

} // namespace api
