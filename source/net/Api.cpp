#include "net/Api.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>

#include "util/Config.h"
#include "util/Log.h"
#include "util/Json.h"

namespace api {

namespace {

constexpr const char* kConfigDir = "sdmc:/switch/fliks";
constexpr const char* kSessionPath = "sdmc:/switch/fliks/session.json";

int64_t nowMs()
{
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string jsonEscape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

std::string readFile(const char* path)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

MediaCard parseCard(const util::Json& m)
{
    MediaCard c;
    c.id = static_cast<int>(m["id"].num());
    c.title = m["title"].str();
    c.year = static_cast<int>(m["year"].num());
    c.series = m["type"].str() == "series";
    c.posterUrl = m["posterUrl"].str();
    c.fanartUrl = m["fanartUrl"].str();
    c.rating = m["rating"].real();
    c.watched = m["watched"].boolean();
    c.progressPercent = static_cast<float>(m["progressPercent"].real());
    if (m["available"].valid()) c.available = m["available"].boolean();
    if (c.year > 0) c.subtitle = std::to_string(c.year);
    return c;
}

} // namespace

std::string switchDeviceProfileJson()
{
    // The ceiling the server picks a rendition against. 12 Mbit suits a
    // server on the same LAN, where decode is what breaks first. Over the
    // internet the link breaks first and by a wide margin: a 6.4 Mbit file
    // across a 2.5 Mbit connection direct-plays at 40% of real time, and no
    // amount of buffering hides that. Lower `maxBitrate` and the server
    // transcodes to something the link can carry.
    const int maxBitrate = util::configInt("maxBitrate", 12000000);
    return R"({
  "directPlayProfiles": [
    { "containers": ["mp4","mkv","mov","webm"],
      "videoCodecs": ["h264","mpeg4","vp8","vp9"],
      "audioCodecs": ["aac","mp3","ac3","flac","vorbis","opus","pcm_s16le"] }
  ],
  "codecConditions": [
    { "codec": "h264", "maxLevel": 41, "profiles": ["baseline","main","high"],
      "maxBitDepth": 8, "maxWidth": 1920, "maxHeight": 1080 },
    { "codec": "vp9", "maxBitDepth": 8, "maxWidth": 1280, "maxHeight": 720 }
  ],
  "maxStreamingBitrate": )" + std::to_string(maxBitrate) + R"(,
  "maxAudioChannels": 2,
  "supportsHdr": false,
  "supportsDolbyVision": false,
  "deviceType": "mobile",
  "deviceName": "Nintendo Switch",
  "systemName": "Horizon",
  "appVersion": ")" APP_VERSION R"("
})";
}

std::string qualityName(const QualityRung& rung)
{
    if (rung.id == "original") return "Original";
    if (rung.id == "auto") return "Auto";
    if (rung.id.rfind("eco-", 0) == 0) return rung.label + " eco";
    return rung.label;
}

std::string qualityDescription(const QualityRung& rung)
{
    std::string out = qualityName(rung);
    if (rung.bitrateBps > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), " · %.2f Mbit/s", rung.bitrateBps / 1.0e6);
        out += buf;
    }
    if (rung.isRemux) out += " (remux)";
    return out;
}

namespace {
std::mutex g_qualitiesMutex;
std::vector<QualityRung> g_qualities;
} // namespace

std::vector<QualityRung> knownQualities()
{
    std::lock_guard<std::mutex> lock(g_qualitiesMutex);
    return g_qualities;
}

void setKnownQualities(std::vector<QualityRung> rungs)
{
    std::lock_guard<std::mutex> lock(g_qualitiesMutex);
    g_qualities = std::move(rungs);
}

void Client::setServer(const std::string& baseUrl)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_server = baseUrl;
    while (!m_server.empty() && m_server.back() == '/') m_server.pop_back();
}

std::string Client::server() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_server;
}

bool Client::hasSession() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_accessToken.empty();
}

User Client::currentUser() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_user;
}

net::Response Client::call(const std::string& method, const std::string& path,
                           const std::string& body, bool retryOn401)
{
    net::Request req;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        req.url = m_server + path;
        if (!m_accessToken.empty())
            req.headers.emplace_back("Authorization", "Bearer " + m_accessToken);
    }
    req.method = method;
    req.body = body;
    if (!body.empty()) req.contentType = "application/json";

    net::Response res = net::perform(req);
    if (res.status == 401 && retryOn401 && refreshTokens())
        return call(method, path, body, false);
    return res;
}

bool Client::refreshTokens()
{
    std::string refresh;
    std::string url;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_refreshToken.empty()) return false;
        refresh = m_refreshToken;
        url = m_server + "/api/auth/refresh";
    }

    net::Request req;
    req.method = "POST";
    req.url = url;
    req.contentType = "application/json";
    req.body = "{\"refreshToken\":\"" + jsonEscape(refresh) + "\"}";

    const net::Response res = net::perform(req);
    if (!res.ok()) return false;

    util::Json root = util::Json::parse(res.body);
    if (!root["accessToken"].hasStr()) return false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_accessToken = root["accessToken"].str();
        if (root["refreshToken"].hasStr()) m_refreshToken = root["refreshToken"].str();
        // The rotated refresh token invalidates the stored one immediately, so
        // the new pair has to hit disk before anything else can fail.
        m_streamToken.clear();
    }
    saveSession();
    return true;
}

bool Client::login(const std::string& username, const std::string& password, std::string& error)
{
    net::Request req;
    req.method = "POST";
    req.url = server() + "/api/auth/login";
    req.contentType = "application/json";
    req.body = "{\"username\":\"" + jsonEscape(username) + "\",\"password\":\"" +
               jsonEscape(password) + "\"}";

    const net::Response res = net::perform(req);
    if (!res.error.empty()) {
        error = res.error;
        return false;
    }
    if (!res.ok()) {
        util::Json root = util::Json::parse(res.body);
        const std::string msg = root["message"].str();
        error = msg.empty() ? ("HTTP " + std::to_string(res.status)) : msg;
        return false;
    }

    util::Json root = util::Json::parse(res.body);
    if (!root["accessToken"].hasStr()) {
        error = "server returned no access token";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_accessToken = root["accessToken"].str();
        m_refreshToken = root["refreshToken"].str();
        util::Json u = root["user"];
        m_user.id = static_cast<int>(u["id"].num());
        m_user.username = u["username"].str();
        m_user.avatar = u["avatar"].str();
        m_user.isAdmin = u["isAdmin"].boolean();
        m_streamToken.clear();
        m_streamTokenExpiresAtMs = 0;
    }
    saveSession();
    return true;
}

bool Client::resumeSession(std::string& error)
{
    const net::Response res = call("GET", "/api/auth/me");
    if (!res.ok()) {
        error = res.error.empty() ? ("HTTP " + std::to_string(res.status)) : res.error;
        return false;
    }
    util::Json u = util::Json::parse(res.body);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_user.id = static_cast<int>(u["id"].num());
    m_user.username = u["username"].str();
    m_user.avatar = u["avatar"].str();
    m_user.isAdmin = u["isAdmin"].boolean();
    return m_user.id != 0;
}

void Client::logout()
{
    call("POST", "/api/auth/logout", "{}", false);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_accessToken.clear();
        m_refreshToken.clear();
        m_streamToken.clear();
        m_user = User{};
    }
    std::remove(kSessionPath);
}

bool Client::loadSession()
{
    const std::string text = readFile(kSessionPath);
    if (text.empty()) return false;
    util::Json root = util::Json::parse(text);
    if (!root.valid()) return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_server = root["server"].str();
    m_accessToken = root["accessToken"].str();
    m_refreshToken = root["refreshToken"].str();
    m_user.id = static_cast<int>(root["userId"].num());
    m_user.username = root["username"].str();
    return !m_server.empty();
}

void Client::saveSession() const
{
    std::string payload;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        payload = "{\"server\":\"" + jsonEscape(m_server) + "\",\"accessToken\":\"" +
                  jsonEscape(m_accessToken) + "\",\"refreshToken\":\"" +
                  jsonEscape(m_refreshToken) + "\",\"userId\":" + std::to_string(m_user.id) +
                  ",\"username\":\"" + jsonEscape(m_user.username) + "\"}";
    }
    ::mkdir("sdmc:/switch", 0777);
    ::mkdir(kConfigDir, 0777);
    FILE* f = std::fopen(kSessionPath, "wb");
    if (!f) return;
    std::fwrite(payload.data(), 1, payload.size(), f);
    std::fclose(f);
}

std::string Client::imageUrl(const std::string& url, const char* size) const
{
    if (url.empty()) return {};
    if (url.rfind("http", 0) == 0) return url;
    std::string resolved = url;
    // `imageUrlWithSize`: the variant param is only meaningful for artwork the
    // server stores itself; a TMDB passthrough URL is left alone.
    if (size && resolved.rfind("/api/images/", 0) == 0)
        resolved += (resolved.find('?') == std::string::npos ? "?" : "&") + std::string("size=") + size;

    std::lock_guard<std::mutex> lock(m_mutex);
    return m_server + resolved;
}

bool Client::fetchBytes(const std::string& absoluteUrl, std::string& out)
{
    net::Request req;
    req.url = absoluteUrl;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_accessToken.empty())
            req.headers.emplace_back("Authorization", "Bearer " + m_accessToken);
    }
    net::Response res = net::perform(req);
    if (!res.ok()) return false;
    out = std::move(res.body);
    return !out.empty();
}

bool Client::fetchLibraries(std::vector<Library>& out)
{
    const net::Response res = call("GET", "/api/libraries/mine");
    if (!res.ok()) return false;
    util::Json arr = util::Json::parse(res.body);
    for (size_t i = 0; i < arr.size(); i++) {
        util::Json l = arr.at(i);
        Library lib;
        lib.id = static_cast<int>(l["id"].num());
        lib.name = l["name"].str();
        lib.icon = l["icon"].str();
        lib.color = l["color"].str();
        util::Json types = l["mediaTypes"];
        for (size_t t = 0; t < types.size(); t++) {
            const std::string v = types.at(t).str();
            if (v == "series") lib.hasSeries = true;
            if (v == "movie") lib.hasMovies = true;
        }
        out.push_back(lib);
    }
    return true;
}

bool Client::fetchContinueWatching(std::vector<MediaCard>& out)
{
    const net::Response res = call("GET", "/api/playback/continue-watching");
    if (!res.ok()) return false;
    util::Json arr = util::Json::parse(res.body);
    for (size_t i = 0; i < arr.size(); i++) {
        util::Json it = arr.at(i);
        MediaCard c;
        c.id = static_cast<int>(it["mediaId"].num());
        c.mediaFileId = static_cast<int>(it["mediaFileId"].num());
        c.episodeId = static_cast<int>(it["episodeId"].num());
        c.title = it["mediaTitle"].str();
        c.subtitle = it["episodeLabel"].str();
        c.series = it["mediaType"].str() == "series";
        c.positionSeconds = it["positionSeconds"].real();
        c.progressPercent = static_cast<float>(it["progressPercent"].real());
        // The rail is landscape, so a still beats the poster where one exists.
        c.posterUrl = it["stillUrl"].hasStr() ? it["stillUrl"].str() : it["fanartUrl"].str();
        if (c.posterUrl.empty()) c.posterUrl = it["posterUrl"].str();
        c.fanartUrl = it["fanartUrl"].str();
        c.landscape = true;
        out.push_back(c);
    }
    return true;
}

bool Client::fetchRecommendations(std::vector<MediaCard>& out)
{
    const net::Response res = call("GET", "/api/playback/recommendations");
    if (!res.ok()) return false;
    util::Json arr = util::Json::parse(res.body);
    for (size_t i = 0; i < arr.size(); i++) {
        util::Json rec = arr.at(i);
        util::Json m = rec["media"];
        if (!m.valid()) continue;
        out.push_back(parseCard(m));
    }
    return true;
}

bool Client::fetchRecentlyAdded(int libraryId, int limit, std::vector<MediaCard>& out)
{
    std::string path = "/api/media/recently-added?limit=" + std::to_string(limit);
    if (libraryId > 0) path += "&libraryId=" + std::to_string(libraryId);
    const net::Response res = call("GET", path);
    if (!res.ok()) return false;
    util::Json arr = util::Json::parse(res.body);
    for (size_t i = 0; i < arr.size(); i++) out.push_back(parseCard(arr.at(i)));
    return true;
}

bool Client::fetchMediaPage(const SearchParams& params, std::vector<MediaCard>& out, int& total)
{
    std::string path = "/api/media?page=" + std::to_string(params.page) +
                       "&limit=" + std::to_string(params.limit) +
                       "&sortBy=" + net::urlEncode(params.sortBy) +
                       "&sortOrder=" + params.sortOrder;
    if (params.libraryId > 0) path += "&libraryId=" + std::to_string(params.libraryId);
    if (!params.q.empty()) path += "&q=" + net::urlEncode(params.q);
    if (params.type) path += std::string("&type=") + params.type;

    const net::Response res = call("GET", path);
    if (!res.ok()) return false;
    util::Json root = util::Json::parse(res.body);
    total = static_cast<int>(root["total"].num());
    util::Json data = root["data"];
    for (size_t i = 0; i < data.size(); i++) out.push_back(parseCard(data.at(i)));
    return true;
}

bool Client::toggleWatched(int mediaId, int mediaFileId, int episodeId, bool& watchedOut)
{
    std::string body = "{";
    if (mediaFileId > 0) body += "\"mediaFileId\":" + std::to_string(mediaFileId);
    if (episodeId > 0) {
        if (body.size() > 1) body += ",";
        body += "\"episodeId\":" + std::to_string(episodeId);
    }
    body += "}";

    const net::Response res =
        call("POST", "/api/playback/media/" + std::to_string(mediaId) + "/toggle-watched", body);
    if (!res.ok()) return false;
    util::Json root = util::Json::parse(res.body);
    watchedOut = root["watched"].boolean();
    return true;
}

bool Client::fetchLiked(int mediaId, bool& likedOut)
{
    const net::Response res = call("GET", "/api/likes/state/" + std::to_string(mediaId));
    if (!res.ok()) return false;
    util::Json root = util::Json::parse(res.body);
    if (!root.valid()) return false;
    likedOut = root["media"].boolean();
    return true;
}

bool Client::setLiked(int mediaId, bool liked)
{
    const std::string body = "{\"mediaId\":" + std::to_string(mediaId) + "}";
    // Same body either way; only the verb differs.
    const net::Response res = call(liked ? "POST" : "DELETE", "/api/likes", body);
    return res.ok();
}

bool Client::fetchPerson(int id, PersonDetail& out)
{
    const net::Response res = call("GET", "/api/persons/" + std::to_string(id));
    if (!res.ok()) return false;
    util::Json root = util::Json::parse(res.body);
    if (!root.valid()) return false;

    util::Json p = root["person"];
    out.id = static_cast<int>(p["id"].num());
    out.name = p["name"].str();
    out.avatarUrl = p["avatarUrl"].str();
    out.biography = p["biography"].str();
    out.birthday = p["birthday"].str();
    out.deathday = p["deathday"].str();
    out.placeOfBirth = p["placeOfBirth"].str();
    out.knownForDepartment = p["knownForDepartment"].str();

    // Both lists carry the same shape, one credit per row, differing only in
    // which field names the role.
    auto credits = [](util::Json list, const char* roleKey, std::vector<PersonCredit>& into) {
        for (size_t i = 0; i < list.size(); i++) {
            util::Json entry = list.at(i);
            util::Json media = entry["media"];
            if (!media.valid()) continue;
            PersonCredit credit;
            credit.media = parseCard(media);
            credit.role = entry[roleKey].str();
            into.push_back(std::move(credit));
        }
    };
    credits(root["cast"], "character", out.cast);
    credits(root["crew"], "job", out.crew);
    FLIKS_LOG("person: %s, %zu cast, %zu crew credits", out.name.c_str(), out.cast.size(),
              out.crew.size());
    return out.id > 0;
}

bool Client::fetchMediaDetail(int id, MediaDetail& out)
{
    const net::Response res = call("GET", "/api/media/" + std::to_string(id));
    if (!res.ok()) return false;
    util::Json m = util::Json::parse(res.body);
    if (!m.valid()) return false;

    out.id = static_cast<int>(m["id"].num());
    out.title = m["title"].str();
    out.overview = m["overview"].str();
    out.posterUrl = m["posterUrl"].str();
    out.fanartUrl = m["fanartUrl"].str();
    out.logoUrl = m["logoUrl"].str();
    out.status = m["status"].str();
    out.year = static_cast<int>(m["year"].num());
    out.runtime = static_cast<int>(m["runtime"].num());
    out.rating = m["rating"].real();
    out.series = m["type"].str() == "series";
    out.watched = m["watched"].boolean();
    out.progressPercent = static_cast<float>(m["progressPercent"].real());
    out.genres = m["genres"].strArray();
    out.libraryName = m["library"]["name"].str();

    util::Json files = m["files"];
    for (size_t i = 0; i < files.size(); i++) {
        util::Json f = files.at(i);
        MediaFile mf;
        mf.id = static_cast<int>(f["id"].num());
        mf.quality = f["quality"].str();
        mf.size = f["size"].num();
        mf.episodeId = static_cast<int>(f["episodeId"].num());
        mf.path = f["relativePath"].str();
        mf.addedAt = f["createdAt"].str();

        util::Json info = f["streamInfo"];
        mf.hasStreamInfo = info.valid();
        mf.bitrateBps = static_cast<int64_t>(info["formatBitRate"].num());
        util::Json videos = info["video"];
        if (videos.size() > 0) {
            util::Json v = videos.at(0);
            mf.video.codec = v["codec"].str();
            mf.video.profile = v["profile"].str();
            mf.video.pixelFormat = v["pixelFormat"].str();
            mf.video.frameRate = v["frameRate"].str();
            mf.video.colorSpace = v["colorSpace"].str();
            mf.video.hdrFormat = v["hdrFormat"].str();
            mf.video.aspectRatio = v["displayAspectRatio"].str();
            mf.video.width = static_cast<int>(v["width"].num());
            mf.video.height = static_cast<int>(v["height"].num());
            mf.video.bitDepth = static_cast<int>(v["bitDepth"].num());
            mf.video.bitrateBps = static_cast<int64_t>(v["bitRate"].num());
        }
        util::Json audios = info["audio"];
        for (size_t j = 0; j < audios.size(); j++) {
            util::Json a = audios.at(j);
            AudioStreamInfo track;
            track.codec = a["codec"].str();
            track.language = a["language"].str();
            track.title = a["title"].str();
            track.channelLayout = a["channelLayout"].str();
            track.channels = static_cast<int>(a["channels"].num());
            track.sampleRate = static_cast<int>(a["sampleRate"].num());
            track.bitrateBps = static_cast<int64_t>(a["bitRate"].num());
            mf.audio.push_back(std::move(track));
        }

        // Two sources, and a file can have both: streams inside the container
        // and rows the server stores beside it (downloaded, OCR'd, uploaded).
        util::Json embedded = f["streamInfo"]["subtitles"];
        for (size_t j = 0; j < embedded.size(); j++) {
            util::Json e = embedded.at(j);
            if (e["isImageBased"].boolean()) continue;
            player::SubtitleTrack track;
            track.streamIndex = static_cast<int>(e["streamIndex"].num());
            track.language = e["language"].str();
            track.title = e["title"].str();
            track.codec = e["codec"].str();
            track.forced = e["forced"].boolean();
            track.hearingImpaired = e["hearingImpaired"].boolean();
            mf.subtitles.push_back(std::move(track));
        }
        util::Json external = f["subtitles"];
        for (size_t j = 0; j < external.size(); j++) {
            util::Json e = external.at(j);
            player::SubtitleTrack track;
            track.subtitleId = static_cast<int>(e["id"].num());
            if (track.subtitleId <= 0) continue;
            track.language = e["language"].str();
            track.title = e["title"].valid() ? e["title"].str() : e["name"].str();
            track.forced = e["forced"].boolean();
            track.hearingImpaired = e["hearingImpaired"].boolean();
            mf.subtitles.push_back(std::move(track));
        }

        out.files.push_back(mf);
    }

    util::Json seasons = m["seasons"];
    for (size_t i = 0; i < seasons.size(); i++) {
        util::Json s = seasons.at(i);
        Season season;
        season.id = static_cast<int>(s["id"].num());
        season.number = static_cast<int>(s["seasonNumber"].num());
        season.posterUrl = s["posterUrl"].str();
        util::Json eps = s["episodes"];
        for (size_t e = 0; e < eps.size(); e++) {
            util::Json ep = eps.at(e);
            Episode episode;
            episode.id = static_cast<int>(ep["id"].num());
            episode.number = static_cast<int>(ep["episodeNumber"].num());
            episode.title = ep["title"].str();
            episode.overview = ep["overview"].str();
            episode.airDate = ep["airDate"].str();
            episode.stillUrl = ep["stillUrl"].str();
            episode.runtime = static_cast<int>(ep["runtime"].num());
            episode.hasFile = ep["hasFile"].boolean();
            season.episodes.push_back(episode);
        }
        out.seasons.push_back(season);
    }

    // Resume point. A movie's file is the only candidate; for a series the
    // endpoint names the episode to pick up.
    const net::Response resume = call("GET", "/api/playback/media/" + std::to_string(id));
    if (resume.ok()) {
        util::Json r = util::Json::parse(resume.body);
        if (r.isObject()) {
            out.resumeFileId = static_cast<int>(r["mediaFileId"].num());
            out.resumeEpisodeId = static_cast<int>(r["episodeId"].num());
            out.resumePositionSeconds = r["positionSeconds"].real();
        }
    }
    if (out.resumeFileId == 0 && !out.series && !out.files.empty())
        out.resumeFileId = out.files.front().id;

    const net::Response castRes = call("GET", "/api/media/" + std::to_string(id) + "/cast");
    if (castRes.ok()) {
        util::Json arr = util::Json::parse(castRes.body);
        for (size_t i = 0; i < arr.size() && i < 20; i++) {
            util::Json c = arr.at(i);
            CastEntry entry;
            entry.character = c["character"].str();
            entry.name = c["person"]["name"].str();
            entry.avatarUrl = c["person"]["avatarUrl"].str();
            entry.personId = static_cast<int>(c["person"]["id"].num());
            out.cast.push_back(entry);
        }
    }
    return true;
}

std::string Client::streamTokenLocked()
{
    return m_streamToken.empty() ? m_accessToken : m_streamToken;
}

bool Client::ensureStreamToken()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // A film outlives the 1h access token, and the playback URL is baked
        // at load time, so the long-lived stream token has to be in hand
        // before the manifest URL is built.
        if (!m_streamToken.empty() && m_streamTokenExpiresAtMs - nowMs() > 5 * 60 * 1000)
            return true;
    }
    const net::Response res = call("POST", "/api/auth/stream-token", "{}");
    if (!res.ok()) return false;
    util::Json root = util::Json::parse(res.body);
    if (!root["streamToken"].hasStr()) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_streamToken = root["streamToken"].str();
    m_streamTokenExpiresAtMs = root["expiresAt"].num();
    return true;
}

std::string Client::subtitleUrl(int mediaFileId, const player::SubtitleTrack& track)
{
    ensureStreamToken();

    std::string token;
    std::string base;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        token = streamTokenLocked();
        base = m_server;
    }
    if (base.empty()) return {};

    std::string path = "/api/stream/" + std::to_string(mediaFileId) + "/subtitles/";
    if (track.streamIndex >= 0) path += "embedded/" + std::to_string(track.streamIndex);
    else if (track.subtitleId > 0) path += std::to_string(track.subtitleId);
    else return {};
    return base + path + "?token=" + net::urlEncode(token);
}

bool Client::fetchPlaybackInfo(int mediaFileId, int startAtSeconds, const std::string& quality,
                               PlaybackInfo& out, std::string& error)
{
    ensureStreamToken();

    std::string token;
    std::string base;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        token = streamTokenLocked();
        base = m_server;
    }

    std::string path = "/api/stream/" + std::to_string(mediaFileId) + "/playback-info?token=" +
                       net::urlEncode(token);
    if (startAtSeconds > 0) path += "&startAt=" + std::to_string(startAtSeconds);
    if (!quality.empty()) path += "&startQuality=" + net::urlEncode(quality);

    const net::Response res = call("POST", path, switchDeviceProfileJson());
    if (!res.ok()) {
        error = res.error.empty() ? ("HTTP " + std::to_string(res.status)) : res.error;
        return false;
    }

    util::Json root = util::Json::parse(res.body);
    out.mediaFileId = mediaFileId;
    out.playMethod = root["playMethod"].str();
    out.sessionId = root["sessionId"].str();
    out.container = root["outputContainer"].str();
    out.quality = root["quality"].str();
    out.durationSeconds = root["source"]["durationSeconds"].real();
    out.width = static_cast<int>(root["source"]["width"].num());
    out.height = static_cast<int>(root["source"]["height"].num());

    // What the negotiation actually settled on. We ask for a rung by height
    // and the server answers with a play method and a bitrate; when those two
    // disagree with the request there is no way to tell from the stream alone.
    FLIKS_LOG("api: playback-info method=%s quality=%s container=%s source=%dx%d",
              out.playMethod.c_str(), out.quality.c_str(), out.container.c_str(), out.width,
              out.height);

    util::Json qualities = root["qualities"];
    for (size_t i = 0; i < qualities.size(); i++) {
        util::Json q = qualities.at(i);
        QualityRung rung;
        rung.id = q["id"].str();
        rung.label = q["label"].str();
        rung.height = static_cast<int>(q["height"].num());
        rung.bitrateBps = q["totalBitrateBps"].num();
        rung.isRemux = q["isRemux"].boolean();
        FLIKS_LOG("api:   rung %s '%s' %dp %.2f Mbit/s%s", rung.id.c_str(), rung.label.c_str(),
                  rung.height, rung.bitrateBps / 1.0e6, rung.isRemux ? " (remux)" : "");
        out.qualities.push_back(rung);
    }
    if (!out.qualities.empty()) setKnownQualities(out.qualities);

    std::string playUrl = root["playUrl"].str();
    if (playUrl.empty()) {
        error = "server returned no play url";
        return false;
    }
    if (playUrl.rfind("http", 0) != 0) playUrl = base + playUrl;
    if (playUrl.find("token=") == std::string::npos)
        playUrl += (playUrl.find('?') == std::string::npos ? "?" : "&") + std::string("token=") +
                   net::urlEncode(token);
    if (!out.sessionId.empty() && playUrl.find("sid=") == std::string::npos)
        playUrl += "&sid=" + net::urlEncode(out.sessionId);
    out.playUrl = playUrl;
    return true;
}

bool Client::reportProgress(int mediaId, int mediaFileId, int episodeId, double positionSeconds,
                            double durationSeconds, const std::string& sessionId,
                            const std::string& state)
{
    std::string body = "{\"positionSeconds\":" + std::to_string(static_cast<int>(positionSeconds)) +
                       ",\"durationSeconds\":" + std::to_string(static_cast<int>(durationSeconds)) +
                       ",\"mediaFileId\":" + std::to_string(mediaFileId);
    if (episodeId > 0) body += ",\"episodeId\":" + std::to_string(episodeId);
    if (!sessionId.empty()) {
        body += ",\"sessionId\":\"" + jsonEscape(sessionId) + "\"";
        if (!state.empty()) body += ",\"state\":\"" + jsonEscape(state) + "\"";
    }
    body += "}";
    const net::Response res =
        call("PUT", "/api/playback/media/" + std::to_string(mediaId) + "/state", body);
    return res.ok();
}

void Client::stopSession(const std::string& sessionId)
{
    if (sessionId.empty()) return;
    call("DELETE", "/api/stream/sessions/" + net::urlEncode(sessionId));
}

} // namespace api
