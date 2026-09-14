#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>

#include <switch.h>

#include "app/App.h"
#include "app/Screens.h"
#include "ui/Scroller.h"

namespace app {

using theme::metric::BodyFont;
using theme::metric::CardLandscapeH;
using theme::metric::CardLandscapeW;
using theme::metric::RadiusBox;
using theme::metric::RailGap;
using theme::metric::RailSpacing;
using theme::metric::RailTitleFont;

namespace {

struct Liveness {
    std::atomic<bool> alive{ true };
};

// `w-[140px]` — the md-to-lg poster width, which is what a 960 CSS px TV
// viewport actually lands on.
constexpr float kPosterW = 140.0f;
constexpr float kPosterH = kPosterW * 1.5f;
constexpr float kGutter = 24.0f;   // gap-6

constexpr uint32_t kKindAction = 300;
constexpr uint32_t kKindSeason = 301;
constexpr uint32_t kKindEpisode = 302;
constexpr uint32_t kKindCast = 303;
constexpr uint32_t kKindFileInfo = 304;

std::string metaLine(const api::MediaDetail& media)
{
    std::string out;
    if (media.year > 0) out = std::to_string(media.year);
    const std::string runtime = ui::formatRuntime(media.runtime);
    if (!runtime.empty()) out += (out.empty() ? "" : "  ·  ") + runtime;
    if (!media.status.empty()) out += (out.empty() ? "" : "  ·  ") + media.status;
    return out;
}

std::string joinGenres(const std::vector<std::string>& genres)
{
    std::string out;
    for (size_t i = 0; i < genres.size(); i++) {
        if (i) out += ", ";
        out += genres[i];
    }
    return out;
}

class DetailScreen final : public Screen {
public:
    explicit DetailScreen(int mediaId)
        : m_live(std::make_shared<Liveness>()), m_mediaId(mediaId)
    {
    }
    ~DetailScreen() override { m_live->alive = false; }

    void onEnter(App& app) override { load(app); }
    void onResume(App& app) override { load(app); }

    std::string backgroundUrl() const override { return m_media.fanartUrl; }
    bool heroPage() const override { return true; }
    std::string title() const override { return m_loaded ? m_media.title : std::string(); }

    void update(App&, float dt) override { m_scroll.animate(dt); }

    bool handleInput(App& app) override
    {
        if (app.drawerOpen() || !m_loaded) return false;
        ui::Input& in = app.input();

        if (ui::touchScroll(in, m_scroll, app.contentRect())) return true;
        if (!in.pressed(HidNpadButton_A)) return false;

        const ui::FocusId id = app.focus().current();
        const uint32_t kind = static_cast<uint32_t>(id >> 32);
        const uint32_t index = static_cast<uint32_t>(id & 0xffffffffu);

        if (kind == kKindAction) {
            if (index == 0) play(app, false);
            else if (index == 1) play(app, true);
            else if (index == 2) toggleWatched(app);
            else if (index == 3) toggleLiked(app);
            return true;
        }
        if (kind == kKindCast && index < m_media.cast.size()) {
            const int personId = m_media.cast[index].personId;
            if (personId > 0) app.push(makePersonScreen(personId));
            return true;
        }
        if (kind == kKindFileInfo) {
            m_fileInfoOpen = !m_fileInfoOpen;
            return true;
        }
        if (kind == kKindSeason && index < m_media.seasons.size()) {
            m_season = static_cast<int>(index);
            return true;
        }
        if (kind == kKindEpisode) {
            playEpisode(app, static_cast<int>(index));
            return true;
        }
        return false;
    }

    void draw(App& app) override
    {
        ui::Context& ctx = app.ui();
        const gfx::Rect content = app.contentRect();

        if (!m_loaded) {
            ui::spinner(ctx, gfx::Rect{ content.cx() - 18, content.cy() - 18, 36, 36 },
                        theme::Primary, ctx.time);
            return;
        }

        ctx.r->pushClip(content);
        ctx.focus->beginContainer(0x7de70000, false);

        float y = content.y + 8.0f - m_scroll.offset();
        y = drawHero(app, content, y);
        y += RailSpacing;
        if (m_media.series) y = drawEpisodes(app, content, y);
        y = drawCast(app, content, y);
        y = drawFileInfo(app, content, y);

        ctx.focus->endContainer();
        ctx.r->popClip();

        const float used = y + m_scroll.offset() - content.y;
        m_scroll.setExtent(used + 24.0f, content.h);
        followFocus(app, content);
    }

private:
    float drawHero(App& app, gfx::Rect content, float y)
    {
        ui::Context& ctx = app.ui();
        const gfx::Rect poster{ content.x, y, kPosterW, kPosterH };

        const int decodeWidth = static_cast<int>(kPosterW * ctx.r->scale() + 0.5f);
        const gfx::TexId tex = m_media.posterUrl.empty()
                                   ? gfx::TexInvalid
                                   : ctx.images->get(m_media.posterUrl, "medium", decodeWidth);
        ctx.r->fillRect(poster, theme::Base300, 12.0f);
        if (tex != gfx::TexInvalid) {
            ctx.r->drawImage(tex, poster, theme::White, 12.0f,
                             ui::coverUv(ctx.r->textureWidth(tex), ctx.r->textureHeight(tex),
                                         poster));
        } else {
            ctx.icons->draw(*ctx.r, ui::Icon::Film,
                            gfx::Rect{ poster.cx() - 24, poster.cy() - 24, 48, 48 },
                            theme::BaseContent.withAlpha(0.3f));
        }

        const float infoX = poster.right() + kGutter;
        const float infoW = content.right() - infoX;
        float iy = y + 4.0f;

        gfx::TextStyle titleStyle;
        titleStyle.size = 30.0f;
        titleStyle.bold = true;
        titleStyle.color = theme::BaseContent;
        ctx.text->drawEllipsized(m_media.title,
                                 gfx::Rect{ infoX, iy, infoW, ctx.text->lineHeight(30.0f) },
                                 titleStyle);
        iy += ctx.text->lineHeight(30.0f) + 10.0f;

        // Metadata row: rating in warning, then the date/runtime/status line,
        // then the library badge.
        float mx = infoX;
        if (m_media.rating > 0) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1f", m_media.rating);
            ctx.icons->draw(*ctx.r, ui::Icon::Star, gfx::Rect{ mx, iy + 2, 14, 14 },
                            theme::Warning);
            gfx::TextStyle rs;
            rs.size = 15.0f;
            rs.bold = true;
            rs.color = theme::Warning;
            ctx.text->draw(buf, mx + 18, iy, rs);
            mx += 18 + ctx.text->measure(buf, 15.0f, true) + 18;
        }
        gfx::TextStyle meta;
        meta.size = 15.0f;
        meta.color = theme::BaseContent.withAlpha(0.6f);
        const std::string line = metaLine(m_media);
        if (!line.empty()) {
            ctx.text->draw(line, mx, iy, meta);
            mx += ctx.text->measure(line, 15.0f, false) + 18;
        }
        // When it would finish if you started now, which is the one piece of
        // metadata that changes depending on when you are looking at it.
        const std::string ends = endsAtLabel();
        if (!ends.empty()) {
            ctx.text->draw(ends, mx, iy, meta);
            mx += ctx.text->measure(ends, 15.0f, false) + 18;
        }
        if (!m_media.libraryName.empty()) {
            // `.badge-primary.badge-outline`
            const float w = ctx.text->measure(m_media.libraryName, 13.0f, false) + 18.0f;
            const gfx::Rect badge{ mx, iy - 1, w, 22 };
            ctx.r->strokeRect(badge, theme::Primary, 11.0f, 1.0f);
            gfx::TextStyle bs;
            bs.size = 13.0f;
            bs.color = theme::Primary;
            ctx.text->drawAligned(m_media.libraryName, badge, bs, ui::Align::Center);
        }
        iy += 26.0f;

        if (!m_media.genres.empty()) {
            gfx::TextStyle gs;
            gs.size = 15.0f;
            gs.color = theme::BaseContent.withAlpha(0.8f);
            ctx.text->drawEllipsized(joinGenres(m_media.genres),
                                     gfx::Rect{ infoX, iy, infoW, ctx.text->lineHeight(15.0f) }, gs);
            iy += ctx.text->lineHeight(15.0f) + 6.0f;
        }

        iy += 14.0f;   // mt-6 before the action row
        iy = drawActions(app, infoX, iy, infoW);

        if (!m_media.overview.empty()) {
            iy += 10.0f;
            gfx::TextStyle os;
            os.size = 16.0f;
            os.color = theme::BaseContent.withAlpha(0.8f);
            iy += ctx.text->drawWrapped(m_media.overview, gfx::Rect{ infoX, iy, infoW, 200 }, os, 4);
        }

        return std::max(poster.bottom(), iy);
    }

    // "Ends at 21:47", from the runtime less whatever has been watched.
    std::string endsAtLabel() const
    {
        if (m_media.runtime <= 0 || m_media.series) return {};
        double remaining = m_media.runtime * 60.0;
        if (m_media.resumePositionSeconds > 30.0) remaining -= m_media.resumePositionSeconds;
        if (remaining <= 0.0) return {};

        std::time_t now = std::time(nullptr);
        now += static_cast<std::time_t>(remaining);
        std::tm local{};
        if (!localtime_r(&now, &local)) return {};
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Ends at %02d:%02d", local.tm_hour, local.tm_min);
        return buf;
    }

    float drawActions(App& app, float x, float y, float width)
    {
        ui::Context& ctx = app.ui();
        const bool resumable = m_media.resumePositionSeconds > 30.0;
        const bool playable = m_media.resumeFileId > 0;
        if (!playable) return y;

        ctx.focus->beginContainer(0x7de70100, true);
        const std::string primaryLabel =
            resumable ? "Resume at " + ui::formatTime(m_media.resumePositionSeconds) : "Play";
        const float primaryW = std::min(ui::buttonWidth(ctx, primaryLabel, true) + 12.0f, width);
        const gfx::Rect primary{ x, y, primaryW, 48.0f };
        ui::button(ctx, ui::focusId(kKindAction, 0), primary, primaryLabel,
                   ui::ButtonStyle::Primary, ui::Icon::Play);

        if (resumable) {
            const std::string label = "From start";
            const float w = ui::buttonWidth(ctx, label, true);
            ui::button(ctx, ui::focusId(kKindAction, 1),
                       gfx::Rect{ primary.right() + 10.0f, y, w, 48.0f }, label,
                       ui::ButtonStyle::Neutral, ui::Icon::RotateCcw);
        }
        // Watched and liked sit beside the transport as icon buttons, the way
        // the client puts them: they are state you flip, not journeys.
        float bx = x;
        if (resumable) {
            const std::string label = "From start";
            bx = primary.right() + 10.0f + ui::buttonWidth(ctx, label, true) + 10.0f;
        } else {
            bx = primary.right() + 10.0f;
        }
        const float round = 48.0f;
        auto iconButton = [&](uint32_t index, ui::Icon icon, bool on, gfx::Color onColor) {
            const gfx::Rect box{ bx, y, round, round };
            const ui::FocusId id = ui::focusId(kKindAction, index);
            ctx.focus->add(id, box);
            if (ctx.focus->isFocused(id)) ui::focusRing(ctx, box, round * 0.5f);
            ctx.r->fillRect(box, on ? onColor : theme::Base300, round * 0.5f);
            ctx.icons->draw(*ctx.r, icon, gfx::Rect{ box.cx() - 10, box.cy() - 10, 20, 20 },
                            on ? theme::White : theme::BaseContent.withAlpha(0.8f));
            bx += round + 10.0f;
        };
        if (bx + round * 2.0f + 10.0f <= x + width) {
            iconButton(2, ui::Icon::Check, m_media.watched, theme::Success);
            iconButton(3, ui::Icon::Heart, m_media.liked, theme::Error);
        }

        ctx.focus->endContainer();

        if (resumable && m_media.progressPercent > 0) {
            const gfx::Rect track{ x, y + 58.0f, std::min(width, 320.0f), 6.0f };
            ctx.r->fillRect(track, theme::Base300, 3.0f);
            gfx::Rect fill = track;
            fill.w = track.w * std::min(m_media.progressPercent, 100.0f) / 100.0f;
            ctx.r->fillRect(fill, theme::Primary, 3.0f);
            return y + 72.0f;
        }
        return y + 56.0f;
    }

    float drawEpisodes(App& app, gfx::Rect content, float y)
    {
        ui::Context& ctx = app.ui();
        if (m_media.seasons.empty()) return y;
        m_season = std::min(m_season, static_cast<int>(m_media.seasons.size()) - 1);

        ctx.focus->beginContainer(0x7de70200, true);
        float x = content.x;
        for (size_t i = 0; i < m_media.seasons.size(); i++) {
            const api::Season& season = m_media.seasons[i];
            const std::string label =
                season.number == 0 ? "Specials" : "Season " + std::to_string(season.number);
            const float w = ctx.text->measure(label, 15.0f, true) + 28.0f;
            const gfx::Rect tab{ x, y, w, 36.0f };
            const ui::FocusId id = ui::focusId(kKindSeason, static_cast<uint32_t>(i));
            const bool focused = ctx.focus->isFocused(id);
            const bool active = static_cast<int>(i) == m_season;
            ctx.focus->add(id, tab);

            if (focused) ui::focusRing(ctx, tab, RadiusBox);
            if (active) ctx.r->fillRect(tab, theme::BaseContent.withAlpha(0.12f), RadiusBox);
            gfx::TextStyle ts;
            ts.size = 15.0f;
            ts.bold = active;
            ts.color = theme::BaseContent.withAlpha(active ? 1.0f : 0.6f);
            ctx.text->drawAligned(label, tab, ts, ui::Align::Center);
            x += w + 8.0f;
        }
        ctx.focus->endContainer();
        y += 36.0f + 16.0f;

        const api::Season& season = m_media.seasons[m_season];
        const float rowH = CardLandscapeH + 16.0f;
        for (size_t i = 0; i < season.episodes.size(); i++) {
            const api::Episode& ep = season.episodes[i];
            const gfx::Rect row{ content.x, y, content.w, CardLandscapeH };
            const ui::FocusId id = ui::focusId(kKindEpisode, static_cast<uint32_t>(i));
            ctx.focus->add(id, row);

            if (row.bottom() > content.y - 40 && row.y < content.bottom() + 40) {
                const bool focused = ctx.focus->isFocused(id);
                const gfx::Rect still{ row.x, row.y, CardLandscapeW, CardLandscapeH };
                if (focused) ui::focusRing(ctx, still, RadiusBox);
                ctx.r->fillRect(still, theme::Base300, RadiusBox);

                const int decodeWidth = static_cast<int>(CardLandscapeW * ctx.r->scale() + 0.5f);
                const gfx::TexId tex = ep.stillUrl.empty()
                                           ? gfx::TexInvalid
                                           : ctx.images->get(ep.stillUrl, "thumb", decodeWidth);
                if (tex != gfx::TexInvalid) {
                    ctx.r->drawImage(tex, still, theme::White, RadiusBox,
                                     ui::coverUv(ctx.r->textureWidth(tex),
                                                 ctx.r->textureHeight(tex), still));
                } else {
                    ctx.icons->draw(*ctx.r, ui::Icon::Film,
                                    gfx::Rect{ still.cx() - 18, still.cy() - 18, 36, 36 },
                                    theme::BaseContent.withAlpha(0.2f));
                }
                if (!ep.hasFile) {
                    ctx.r->fillRect(still, theme::Base100.withAlpha(0.55f), RadiusBox);
                    ctx.icons->draw(*ctx.r, ui::Icon::CircleX,
                                    gfx::Rect{ still.cx() - 12, still.cy() - 12, 24, 24 },
                                    theme::Error);
                }

                const float tx = still.right() + 16.0f;
                const float tw = row.right() - tx;
                gfx::TextStyle num;
                num.size = 13.0f;
                num.color = theme::BaseContent.withAlpha(0.5f);
                ctx.text->draw("Episode " + std::to_string(ep.number), tx, row.y + 2, num);

                gfx::TextStyle ts;
                ts.size = 18.0f;
                ts.bold = true;
                ts.color = theme::BaseContent;
                ctx.text->drawEllipsized(ep.title.empty() ? "Untitled" : ep.title,
                                         gfx::Rect{ tx, row.y + 20, tw, ctx.text->lineHeight(18.0f) },
                                         ts);

                if (!ep.overview.empty()) {
                    gfx::TextStyle os;
                    os.size = 14.0f;
                    os.color = theme::BaseContent.withAlpha(0.6f);
                    ctx.text->drawWrapped(ep.overview,
                                          gfx::Rect{ tx, row.y + 46, tw, CardLandscapeH - 46 }, os,
                                          3);
                }
            }
            y += rowH;
        }
        return y + 8.0f;
    }

    // The file the player will actually open: the one behind the resume
    // point, or the only one a film has.
    const api::MediaFile* currentFile() const
    {
        const api::MediaFile* first = nullptr;
        for (const api::MediaFile& f : m_media.files) {
            if (!first) first = &f;
            if (m_media.resumeFileId > 0 && f.id == m_media.resumeFileId) return &f;
        }
        return first;
    }

    static std::string humanSize(int64_t bytes)
    {
        if (bytes <= 0) return {};
        char buf[32];
        if (bytes >= 1024LL * 1024 * 1024)
            std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        else
            std::snprintf(buf, sizeof(buf), "%.0f MB", bytes / (1024.0 * 1024.0));
        return buf;
    }

    static std::string humanBitrate(int64_t bps)
    {
        if (bps <= 0) return {};
        char buf[32];
        if (bps >= 1000000) std::snprintf(buf, sizeof(buf), "%.1f Mbps", bps / 1.0e6);
        else std::snprintf(buf, sizeof(buf), "%lld kbps", static_cast<long long>(bps / 1000));
        return buf;
    }

    // "2026-08-04T12:52:31.000Z" -> "4 August 2026, 12:52". Parsed by position
    // rather than with strptime, which newlib does not carry.
    static std::string humanDate(const std::string& iso)
    {
        if (iso.size() < 16) return iso;
        static const char* kMonths[12] = { "January", "February", "March",     "April",
                                           "May",     "June",     "July",      "August",
                                           "September", "October", "November", "December" };
        const int year = std::atoi(iso.substr(0, 4).c_str());
        const int month = std::atoi(iso.substr(5, 2).c_str());
        const int day = std::atoi(iso.substr(8, 2).c_str());
        if (month < 1 || month > 12) return iso;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d %s %d, %s", day, kMonths[month - 1], year,
                      iso.substr(11, 5).c_str());
        return buf;
    }

    // "5.1(side)" and "48000" are what the probe stored; these are what a
    // person reads.
    static std::string channelName(const api::AudioStreamInfo& track)
    {
        if (!track.channelLayout.empty()) return track.channelLayout;
        if (track.channels == 1) return "mono";
        if (track.channels == 2) return "stereo";
        if (track.channels > 0) return std::to_string(track.channels) + " ch";
        return {};
    }

    void toggleWatched(App& app)
    {
        // Flipped locally first so the button answers immediately; the server
        // decides, and its answer replaces the guess when it lands.
        const bool want = !m_media.watched;
        m_media.watched = want;
        if (want) m_media.progressPercent = 0.0f;

        auto live = m_live;
        auto* tasks = &app.tasks();
        App* appPtr = &app;
        const int mediaId = m_media.id;
        const int fileId = m_media.resumeFileId;
        const int episodeId = m_media.resumeEpisodeId;
        tasks->post([this, live, tasks, appPtr, mediaId, fileId, episodeId, want] {
            bool watched = want;
            const bool ok = appPtr->client().toggleWatched(mediaId, fileId, episodeId, watched);
            tasks->postToMain([this, live, ok, watched, want] {
                if (!live->alive) return;
                m_media.watched = ok ? watched : !want;
            });
        });
    }

    void toggleLiked(App& app)
    {
        const bool want = !m_media.liked;
        m_media.liked = want;

        auto live = m_live;
        auto* tasks = &app.tasks();
        App* appPtr = &app;
        const int mediaId = m_media.id;
        tasks->post([this, live, tasks, appPtr, mediaId, want] {
            const bool ok = appPtr->client().setLiked(mediaId, want);
            if (ok) return;
            tasks->postToMain([this, live, want] {
                if (!live->alive) return;
                m_media.liked = !want;
            });
        });
    }

    float drawFileInfo(App& app, gfx::Rect content, float y)
    {
        const api::MediaFile* file = currentFile();
        if (!file) return y;
        ui::Context& ctx = app.ui();

        y += 12.0f;
        const gfx::Rect header{ content.x, y, content.w, 34.0f };
        const ui::FocusId id = ui::focusId(kKindFileInfo, 0);
        ctx.focus->add(id, header);
        if (ctx.focus->isFocused(id)) ui::focusRing(ctx, header, theme::metric::RadiusField);

        gfx::TextStyle title;
        title.size = RailTitleFont;
        title.bold = true;
        title.color = theme::BaseContent;
        ctx.text->draw("File information", header.x, header.cy() - ctx.text->lineHeight(RailTitleFont) * 0.5f,
                       title);
        ctx.icons->draw(*ctx.r, m_fileInfoOpen ? ui::Icon::ChevronUp : ui::Icon::ChevronDown,
                        gfx::Rect{ header.x + ctx.text->measure("File information", RailTitleFont, true) + 10.0f,
                                   header.cy() - 8.0f, 16, 16 },
                        theme::BaseContent.withAlpha(0.6f));
        y = header.bottom() + 6.0f;
        if (!m_fileInfoOpen) return y;

        // Four columns at 960 wide, the way the web panel lays out; two when
        // there is not room for four.
        const int columns = content.w >= 720.0f ? 4 : 2;
        const float colW = content.w / static_cast<float>(columns);
        const float lineH = 22.0f;
        int slot = 0;

        auto field = [&](const char* label, const std::string& value) {
            if (value.empty()) return;
            const int col = slot % columns;
            const float fx = content.x + col * colW;
            const float fy = y + (slot / columns) * lineH;
            slot++;

            gfx::TextStyle lt;
            lt.size = 13.0f;
            lt.color = theme::BaseContent.withAlpha(0.45f);
            const std::string prefix = std::string(label) + " : ";
            ctx.text->draw(prefix, fx, fy, lt);

            gfx::TextStyle vt;
            vt.size = 13.0f;
            vt.bold = true;
            vt.color = theme::BaseContent.withAlpha(0.85f);
            const float lw = ctx.text->measure(prefix, 13.0f, false);
            ctx.text->drawEllipsized(value, gfx::Rect{ fx + lw, fy, colW - lw - 12.0f, lineH }, vt);
        };
        auto endRow = [&] {
            if (slot % columns != 0) slot += columns - (slot % columns);
        };
        auto section = [&](ui::Icon icon, const char* name) {
            endRow();
            float sy = y + (slot / columns) * lineH + 8.0f;
            ctx.icons->draw(*ctx.r, icon, gfx::Rect{ content.x, sy + 1.0f, 15, 15 },
                            theme::BaseContent.withAlpha(0.7f));
            gfx::TextStyle st;
            st.size = 14.0f;
            st.bold = true;
            st.color = theme::BaseContent.withAlpha(0.9f);
            ctx.text->draw(name, content.x + 22.0f, sy, st);
            slot += columns;   // the heading owns a row of its own
        };

        // The path is the one field that needs the full width, so it gets its
        // own row rather than a quarter of one.
        {
            gfx::TextStyle lt;
            lt.size = 13.0f;
            lt.color = theme::BaseContent.withAlpha(0.45f);
            ctx.text->draw("File : ", content.x, y, lt);
            gfx::TextStyle vt;
            vt.size = 13.0f;
            vt.color = theme::BaseContent.withAlpha(0.85f);
            const float lw = ctx.text->measure("File : ", 13.0f, false);
            ctx.text->drawEllipsized(file->path,
                                     gfx::Rect{ content.x + lw, y, content.w - lw, lineH }, vt);
        }
        slot = columns;   // start the grid on the next row
        field("Added", humanDate(file->addedAt));
        endRow();
        field("Quality", file->quality);
        field("Size", humanSize(file->size));
        field("Bitrate", humanBitrate(file->bitrateBps));

        if (file->hasStreamInfo && !file->video.codec.empty()) {
            const api::VideoStreamInfo& v = file->video;
            section(ui::Icon::Film, "Video");
            field("Codec", v.codec);
            field("Profile", v.profile);
            if (v.width > 0 && v.height > 0) {
                char res[48];
                std::snprintf(res, sizeof(res), "%d×%d", v.width, v.height);
                field("Resolution", res);
            }
            field("Aspect ratio", v.aspectRatio);
            field("Frame rate", v.frameRate.empty() ? std::string() : v.frameRate + " fps");
            field("Pixel format", v.pixelFormat);
            field("Bit depth", v.bitDepth > 0 ? std::to_string(v.bitDepth) + " bit" : std::string());
            field("Video range", v.hdrFormat.empty() ? "SDR" : v.hdrFormat);
            field("Colour space", v.colorSpace);
        }

        if (!file->audio.empty()) {
            section(ui::Icon::Tv, "Audio");
            for (const api::AudioStreamInfo& a : file->audio) {
                field("Track", a.title.empty() ? channelName(a) : a.title);
                field("Language", a.language);
                field("Codec", a.codec);
                field("Channels", channelName(a));
                field("Bitrate", humanBitrate(a.bitrateBps));
                field("Sample rate",
                      a.sampleRate > 0 ? std::to_string(a.sampleRate / 1000) + " kHz"
                                       : std::string());
                endRow();
            }
        }

        endRow();
        return y + (slot / columns) * lineH + 12.0f;
    }

    float drawCast(App& app, gfx::Rect content, float y)
    {
        if (m_media.cast.empty()) return y;
        ui::Context& ctx = app.ui();

        ui::railTitle(ctx, "Cast", gfx::Rect{ content.x, y, content.w, 24.0f });
        y += ctx.text->lineHeight(RailTitleFont) + theme::metric::RailTitleGap;

        constexpr float kAvatar = 96.0f;
        const float cellH = kAvatar + 44.0f;
        ctx.focus->beginContainer(0x7de70300, true);
        ctx.r->pushClip(gfx::Rect{ content.x - 8, y - 10, content.w + 16, cellH + 20 });

        for (size_t i = 0; i < m_media.cast.size(); i++) {
            const api::CastEntry& entry = m_media.cast[i];
            const float x = content.x + i * (kAvatar + RailGap) - m_castScroll.offset();
            const gfx::Rect box{ x, y, kAvatar, cellH };
            const ui::FocusId id = ui::focusId(kKindCast, static_cast<uint32_t>(i));
            ctx.focus->add(id, gfx::Rect{ content.x + i * (kAvatar + RailGap), y, kAvatar, cellH });

            if (box.right() < content.x - 40 || box.x > content.right() + 40) continue;
            const gfx::Rect avatar{ box.x, box.y, kAvatar, kAvatar };
            if (ctx.focus->isFocused(id)) ui::focusRing(ctx, avatar, kAvatar * 0.5f);
            ctx.r->fillRect(avatar, theme::Base300, kAvatar * 0.5f);

            const int decodeWidth = static_cast<int>(kAvatar * ctx.r->scale() + 0.5f);
            const gfx::TexId tex = entry.avatarUrl.empty()
                                       ? gfx::TexInvalid
                                       : ctx.images->get(entry.avatarUrl, "thumb", decodeWidth);
            if (tex != gfx::TexInvalid) {
                ctx.r->drawImage(tex, avatar, theme::White, kAvatar * 0.5f,
                                 ui::coverUv(ctx.r->textureWidth(tex), ctx.r->textureHeight(tex),
                                             avatar));
            } else {
                ctx.icons->draw(*ctx.r, ui::Icon::User,
                                gfx::Rect{ avatar.cx() - 20, avatar.cy() - 20, 40, 40 },
                                theme::BaseContent.withAlpha(0.3f));
            }

            gfx::TextStyle name;
            name.size = 13.0f;
            name.color = theme::BaseContent;
            ctx.text->drawEllipsized(entry.name, gfx::Rect{ box.x - 6, avatar.bottom() + 6,
                                                            kAvatar + 12, 18 },
                                     name, ui::Align::Center);
            gfx::TextStyle role;
            role.size = 12.0f;
            role.color = theme::BaseContent.withAlpha(0.5f);
            ctx.text->drawEllipsized(entry.character,
                                     gfx::Rect{ box.x - 6, avatar.bottom() + 24, kAvatar + 12, 18 },
                                     role, ui::Align::Center);
        }

        ctx.r->popClip();
        ctx.focus->endContainer();
        m_castScroll.setExtent(m_media.cast.size() * (kAvatar + RailGap) - RailGap, content.w);
        m_castScroll.animate(app.ui().dt);

        const ui::FocusId focused = app.focus().current();
        if (static_cast<uint32_t>(focused >> 32) == kKindCast) {
            if (!app.input().touchDriven())
                m_castScroll.revealX(app.focus().rectOf(focused),
                                     gfx::Rect{ content.x, y, content.w, cellH });
        }
        return y + cellH;
    }

    void followFocus(App& app, gfx::Rect content)
    {
        if (app.input().touchDriven()) return;
        const ui::FocusId id = app.focus().current();
        if (id == ui::FocusNone) return;
        const gfx::Rect item = app.focus().rectOf(id);
        if (item.empty()) return;
        // The rect is already in scrolled coordinates, so undo the offset to
        // get the position within the page.
        m_scroll.revealY(gfx::Rect{ item.x, item.y + m_scroll.offset(), item.w, item.h }, content,
                         24.0f);
    }

    void play(App& app, bool fromStart)
    {
        if (m_media.resumeFileId <= 0) return;
        PlayRequest req;
        req.mediaId = m_media.id;
        req.mediaFileId = m_media.resumeFileId;
        req.episodeId = m_media.resumeEpisodeId;
        req.startAtSeconds = fromStart ? 0.0 : m_media.resumePositionSeconds;
        req.title = m_media.title;
        app.push(makePlayerScreen(req));
    }

    void playEpisode(App& app, int index)
    {
        if (m_season < 0 || m_season >= static_cast<int>(m_media.seasons.size())) return;
        const api::Season& season = m_media.seasons[m_season];
        if (index < 0 || index >= static_cast<int>(season.episodes.size())) return;
        const api::Episode& ep = season.episodes[index];
        if (!ep.hasFile) {
            app.toast("That episode is not on disk", true);
            return;
        }

        int fileId = 0;
        for (const api::MediaFile& f : m_media.files)
            if (f.episodeId == ep.id) fileId = f.id;
        if (fileId == 0) {
            app.toast("No file for that episode", true);
            return;
        }

        PlayRequest req;
        req.mediaId = m_media.id;
        req.mediaFileId = fileId;
        req.episodeId = ep.id;
        req.startAtSeconds = (m_media.resumeEpisodeId == ep.id) ? m_media.resumePositionSeconds : 0.0;
        req.title = m_media.title;
        char label[64];
        std::snprintf(label, sizeof(label), "S%02dE%02d", season.number, ep.number);
        req.subtitle = std::string(label) + (ep.title.empty() ? "" : " · " + ep.title);
        app.push(makePlayerScreen(req));
    }

    void load(App& app)
    {
        auto live = m_live;
        auto* tasks = &app.tasks();
        App* appPtr = &app;
        const int id = m_mediaId;

        tasks->post(
            [this, live, tasks, appPtr, id] {
                auto detail = std::make_shared<api::MediaDetail>();
                const bool ok = appPtr->client().fetchMediaDetail(id, *detail);
                // The heart is a second call: likes live apart from the media
                // record, so the detail response knows nothing about them.
                if (ok) appPtr->client().fetchLiked(id, detail->liked);
                tasks->postToMain([this, live, appPtr, detail, ok] {
                    if (!live->alive) return;
                    if (!ok) {
                        appPtr->toast("Could not load that title", true);
                        return;
                    }
                    m_media = *detail;
                    // The season that holds the resume point is the one worth
                    // opening on.
                    for (size_t s = 0; s < m_media.seasons.size(); s++)
                        for (const api::Episode& ep : m_media.seasons[s].episodes)
                            if (ep.id == m_media.resumeEpisodeId) m_season = static_cast<int>(s);
                    m_loaded = true;
                });
            },
            util::TaskQueue::Priority::High);
    }

    std::shared_ptr<Liveness> m_live;
    api::MediaDetail m_media;
    ui::Scroller m_scroll;
    ui::Scroller m_castScroll;
    int m_mediaId = 0;
    int m_season = 0;
    bool m_loaded = false;
    // Open on arrival — the panel is reference material people come to the
    // page for, not something to go looking for. The header still collapses
    // it when the cast rail is what matters.
    bool m_fileInfoOpen = true;
};

} // namespace

std::unique_ptr<Screen> makeDetailScreen(int mediaId)
{
    return std::make_unique<DetailScreen>(mediaId);
}

} // namespace app
