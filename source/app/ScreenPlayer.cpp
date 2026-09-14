#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <vector>

#include <switch.h>

extern "C" {
#include <libavutil/frame.h>
}

#include "app/App.h"
#include "app/Screens.h"
#include "player/HlsVariant.h"
#include "player/Player.h"
#include "player/Subtitles.h"
#include "player/VideoRenderer.h"
#include "util/Config.h"
#include "util/Log.h"

namespace app {

namespace {

struct Liveness {
    std::atomic<bool> alive{ true };
};

constexpr float kControlsTimeout = 4.0f;
constexpr double kSeekStep = 10.0;
constexpr double kSeekStepLarge = 60.0;
// Long enough that a burst of taps lands as one seek, short enough that a
// single tap still feels immediate.
constexpr float kSeekCommitDelay = 0.45f;
// A tap on the left or right third, twice, seeks — the gesture every phone
// player uses. Beyond this gap the second tap is just another tap.
constexpr float kDoubleTapWindow = 0.4f;
constexpr double kHeartbeatSeconds = 10.0;

class PlayerScreen final : public Screen {
    // One overlay serves all three lists; which one is showing decides where
    // a selection goes. Three near-identical menus would have been three
    // places to fix the next layout problem. Declared up here because member
    // signatures below name it, and those are looked up in order.
    enum class Menu { None, Quality, Audio, Subtitles };

public:
    explicit PlayerScreen(PlayRequest request)
        : m_live(std::make_shared<Liveness>()), m_request(std::move(request))
    {
    }

    ~PlayerScreen() override { m_live->alive = false; }

    bool showsChrome() const override { return false; }

    void onEnter(App& app) override
    {
        appletSetMediaPlaybackState(true);
        if (!m_video.init(app.renderer())) FLIKS_LOG("player: video renderer init failed");
        resolveStream(app, m_request.startAtSeconds);
        loadSubtitleTracks(app);
    }

    // The list lives on the media detail, which the continue rail never
    // loaded — so it is fetched here rather than threaded through every
    // screen that can start playback.
    void loadSubtitleTracks(App& app)
    {
        if (m_request.mediaId <= 0) return;
        auto live = m_live;
        auto* tasks = &app.tasks();
        App* appPtr = &app;
        const int mediaId = m_request.mediaId;
        const int fileId = m_request.mediaFileId;
        tasks->post([this, live, tasks, appPtr, mediaId, fileId] {
            api::MediaDetail detail;
            if (!appPtr->client().fetchMediaDetail(mediaId, detail)) return;
            std::vector<player::SubtitleTrack> tracks;
            for (const api::MediaFile& file : detail.files)
                if (file.id == fileId) tracks = file.subtitles;
            tasks->postToMain([this, live, tracks] {
                if (!live->alive) return;
                m_subtitleTracks = tracks;
                for (const player::SubtitleTrack& t : m_subtitleTracks)
                    FLIKS_LOG("subs: track %s", t.label().c_str());
            });
        });
    }

    void onExit(App& app) override
    {
        reportProgress(app, "stopped");
        m_player.close();
        if (m_frame) {
            av_frame_free(&m_frame);
            m_frame = nullptr;
        }
        m_video.shutdown(app.renderer());
        stopSession(app);
        appletSetMediaPlaybackState(false);
    }

    void update(App& app, float dt) override
    {
        if (m_controlsTimer > 0.0f && m_player.state() == player::State::Playing)
            m_controlsTimer -= dt;

        m_player.advanceClock(dt);
        // Refreshed once a frame: audioTracks() copies under a lock, and the
        // draw path asked for it three times.
        m_audioCache = m_player.audioTracks();

        // A pending seek commits once the presses stop. Scrubbing holds it
        // open until the finger lifts instead.
        if (m_pendingSeek >= 0.0 && !m_scrubbing && m_seekCommit > 0.0f) {
            m_seekCommit -= dt;
            if (m_seekCommit <= 0.0f) commitSeek();
        }

        m_heartbeat += dt;
        if (m_heartbeat >= kHeartbeatSeconds) {
            m_heartbeat = 0.0f;
            reportProgress(app, m_player.paused() ? "paused" : "playing");
        }

        if (m_player.state() == player::State::Ended) app.pop();
        if (m_player.state() == player::State::Failed && !m_reportedFailure) {
            m_reportedFailure = true;
            app.toast(m_player.error().empty() ? "Playback failed" : m_player.error(), true);
        }
    }

    bool handleInput(App& app) override
    {
        ui::Input& in = app.input();
        const bool wasVisible = controlsVisible();

        if (m_menu != Menu::None) return handleMenuInput(app);
        if (handleTouch(app)) return true;

        if (in.pressed(HidNpadButton_B)) {
            app.pop();
            return true;
        }
        if (in.anyNavigate() || in.pressed(HidNpadButton_A) || in.pressed(HidNpadButton_X) ||
            in.pressed(HidNpadButton_Y))
            m_controlsTimer = kControlsTimeout;

        if (in.pressed(HidNpadButton_X)) {
            player::Settings s = player::settings();
            s.showStats = !s.showStats;
            player::setSettings(s);
            return true;
        }

        // The first press only wakes the controls, the way the client's
        // overlay swallows the tap that reveals it.
        if (!wasVisible) return true;

        if (in.pressed(HidNpadButton_Y)) {
            openMenu(defaultMenu());
            return true;
        }
        if (in.pressed(HidNpadButton_A)) {
            m_player.togglePause();
            reportProgress(app, m_player.paused() ? "paused" : "playing");
            return true;
        }
        if (in.navigate(ui::Dir::Left)) {
            nudgeSeek(-kSeekStep);
            return true;
        }
        if (in.navigate(ui::Dir::Right)) {
            nudgeSeek(kSeekStep);
            return true;
        }
        if (in.pressed(HidNpadButton_L)) {
            nudgeSeek(-kSeekStepLarge);
            return true;
        }
        if (in.pressed(HidNpadButton_R)) {
            nudgeSeek(kSeekStepLarge);
            return true;
        }
        return true;
    }

    // Every seek goes through here. Holding Right used to fire one seek per
    // repeat tick, and on a fed stream a seek tears the session down and
    // rebuilds it — so a two-second press meant a dozen rebuilds and several
    // seconds of black. Now the target accumulates and commits once the
    // presses stop, which also gives the scrubber something to preview.
    void nudgeSeek(double delta)
    {
        const double from = m_pendingSeek >= 0.0 ? m_pendingSeek : m_player.position();
        const double duration = effectiveDuration();
        double target = from + delta;
        if (target < 0.0) target = 0.0;
        if (duration > 0.0 && target > duration - 1.0) target = std::max(0.0, duration - 1.0);
        m_pendingSeek = target;
        m_seekCommit = kSeekCommitDelay;
        m_controlsTimer = kControlsTimeout;
    }

    void commitSeek()
    {
        if (m_pendingSeek < 0.0) return;
        m_player.seekTo(m_pendingSeek);
        m_pendingSeek = -1.0;
        m_seekCommit = 0.0f;
    }

    double effectiveDuration() const
    {
        return m_player.duration() > 0 ? m_player.duration() : m_declaredDuration;
    }

    // Where the bar should read, which during a pending seek is the target
    // rather than the stale decoder position.
    double displayPosition() const
    {
        if (m_pendingSeek >= 0.0) return m_pendingSeek;
        return m_player.position();
    }

    void draw(App& app) override
    {
        ui::Context& ctx = app.ui();
        const gfx::Rect screen{ 0, 0, ctx.r->width(), ctx.r->height() };
        ctx.r->fillRect(screen, theme::Black, 0.0f);

        // Taken here rather than in update(): every GPU command for a frame
        // then sits inside one begin/end window, so the staging a copy reads
        // from is never recycled by the same frame that recorded it.
        if (AVFrame* frame = m_player.acquireFrame()) {
            if (m_frame) av_frame_free(&m_frame);
            m_frame = frame;
            m_video.upload(*ctx.r, m_frame);
        }

        m_traceFrames++;
        if (util::configInt("trace", 0) != 0 && (m_traceFrames <= 20 || m_traceFrames % 30 == 0)) {
            FLIKS_LOG("play %u: state=%d pos=%.2f queued=%d planes=%d dropped=%d", m_traceFrames,
                      static_cast<int>(m_player.state()), m_player.position(),
                      m_player.queuedFrames(), m_video.hasFrame() ? 1 : 0,
                      m_player.droppedFrames());
        }

        if (m_video.hasFrame()) m_video.draw(*ctx.r, m_video.fit(screen));

        const player::State state = m_player.state();
        if (state == player::State::Opening || (!m_video.hasFrame() && state != player::State::Failed))
            drawLoading(app, screen);
        if (state == player::State::Failed) drawError(app, screen);

        if (controlsVisible()) drawControls(app, screen);
        if (!m_subtitles.empty()) drawSubtitles(app, screen);
        if (player::settings().showStats) drawStats(app, screen);
        if (m_menu != Menu::None) drawMenu(app, screen);
    }

private:
    // Returns true when the touch was the interaction for this frame, so the
    // buttons below never also fire.
    bool handleTouch(App& app)
    {
        ui::Input& in = app.input();
        if (!in.touchActive() && !in.touchUp()) return false;

        const gfx::Rect screen{ 0, 0, app.ui().r->width(), app.ui().r->height() };
        const bool visible = controlsVisible();

        // Dragging the scrubber, once it has been grabbed, continues wherever
        // the finger goes — including off the track, which is what makes a
        // thin bar usable at all.
        if (m_scrubbing) {
            if (in.touchUp()) {
                m_scrubbing = false;
                commitSeek();
            } else {
                scrubTo(in.touchX());
            }
            m_controlsTimer = kControlsTimeout;
            return true;
        }

        if (visible && in.pressedIn(m_hitTrack.expand(14.0f))) {
            m_scrubbing = true;
            scrubTo(in.touchX());
            m_controlsTimer = kControlsTimeout;
            return true;
        }

        if (!in.touchUp()) return in.touchActive();

        if (visible) {
            if (in.tapped(m_hitPlay)) {
                m_player.togglePause();
                reportProgress(app, m_player.paused() ? "paused" : "playing");
                m_controlsTimer = kControlsTimeout;
                return true;
            }
            if (in.tapped(m_hitBack10)) {
                nudgeSeek(-kSeekStep);
                return true;
            }
            if (in.tapped(m_hitFwd10)) {
                nudgeSeek(kSeekStep);
                return true;
            }
            if (in.tapped(m_hitQuality)) {
                openMenu(Menu::Quality);
                return true;
            }
            if (in.tapped(m_hitAudio)) {
                openMenu(Menu::Audio);
                return true;
            }
            if (in.tapped(m_hitSubs)) {
                openMenu(Menu::Subtitles);
                return true;
            }
            if (in.tapped(m_hitClose)) {
                app.pop();
                return true;
            }
        }

        if (!in.tapped(screen)) return true;

        // A second tap in the same third seeks; the first one is held back
        // until the window closes so a single tap still toggles the controls.
        const float third = screen.w / 3.0f;
        const int zone = in.touchX() < third ? 0 : (in.touchX() > screen.w - third ? 2 : 1);
        if (zone != 1 && m_lastTapZone == zone && app.ui().time - m_lastTapTime < kDoubleTapWindow) {
            nudgeSeek(zone == 0 ? -kSeekStep : kSeekStep);
            m_lastTapZone = -1;
            return true;
        }
        m_lastTapZone = zone;
        m_lastTapTime = app.ui().time;

        // Otherwise a tap is the show/hide the rest of the UI expects.
        m_controlsTimer = visible ? 0.0f : kControlsTimeout;
        return true;
    }

    void scrubTo(float x)
    {
        const double duration = effectiveDuration();
        if (duration <= 0 || m_hitTrack.w <= 0) return;
        float t = (x - m_hitTrack.x) / m_hitTrack.w;
        t = std::min(1.0f, std::max(0.0f, t));
        m_pendingSeek = duration * t;
        // A drag commits on release, not on a timer.
        m_seekCommit = m_scrubbing ? 0.0f : kSeekCommitDelay;
    }

    Menu defaultMenu() const
    {
        if (!m_qualities.empty()) return Menu::Quality;
        if (m_audioCache.size() > 1) return Menu::Audio;
        return Menu::Subtitles;
    }

    void openMenu(Menu menu)
    {
        m_menu = menu;
        m_menuIndex = 0;
        m_controlsTimer = kControlsTimeout;
        if (menu == Menu::Quality) {
            for (size_t i = 0; i < m_qualities.size(); i++)
                if (m_qualities[i].id == m_quality) m_menuIndex = static_cast<int>(i);
        } else if (menu == Menu::Audio) {
            for (size_t i = 0; i < m_audioCache.size(); i++)
                if (m_audioCache[i].index == m_player.currentAudioTrack())
                    m_menuIndex = static_cast<int>(i);
        } else if (menu == Menu::Subtitles) {
            m_menuIndex = m_subtitleTrack + 1;   // row 0 is "Off"
        }
    }

    int menuRowCount() const
    {
        switch (m_menu) {
            case Menu::Quality: return static_cast<int>(m_qualities.size());
            case Menu::Audio: return static_cast<int>(m_audioCache.size());
            // Row 0 is "Off", which is always offered.
            case Menu::Subtitles: return static_cast<int>(m_subtitleTracks.size()) + 1;
            default: return 0;
        }
    }

    bool handleMenuInput(App& app)
    {
        ui::Input& in = app.input();
        const int count = menuRowCount();
        if (count <= 0) {
            m_menu = Menu::None;
            return true;
        }

        if (in.touchUp()) {
            if (in.tapped(m_hitClose)) {
                m_menu = Menu::None;
                m_controlsTimer = kControlsTimeout;
                return true;
            }
            if (m_menuRowH > 0 && in.tapped(m_hitMenuRows)) {
                const int row = static_cast<int>((in.touchY() - m_hitMenuRows.y) / m_menuRowH);
                if (row >= 0 && row < count) {
                    m_menuIndex = row;
                    applyMenuChoice(app);
                }
                return true;
            }
            // Anywhere outside dismisses, which is what a tap off a sheet
            // means everywhere else.
            m_menu = Menu::None;
            m_controlsTimer = kControlsTimeout;
            return true;
        }
        if (in.touchActive()) return true;

        if (in.pressed(HidNpadButton_B) || in.pressed(HidNpadButton_Y)) {
            m_menu = Menu::None;
            m_controlsTimer = kControlsTimeout;
            return true;
        }
        // Left and Right step between the three lists, so reaching subtitles
        // is never more than one press from wherever the menu opened.
        if (in.navigate(ui::Dir::Left) || in.navigate(ui::Dir::Right)) {
            cycleMenu(in.navigate(ui::Dir::Right) ? 1 : -1);
            return true;
        }
        if (in.navigate(ui::Dir::Up)) {
            m_menuIndex = (m_menuIndex + count - 1) % count;
            return true;
        }
        if (in.navigate(ui::Dir::Down)) {
            m_menuIndex = (m_menuIndex + 1) % count;
            return true;
        }
        if (in.pressed(HidNpadButton_A)) {
            applyMenuChoice(app);
            return true;
        }
        return true;
    }

    void cycleMenu(int step)
    {
        const Menu order[3] = { Menu::Quality, Menu::Audio, Menu::Subtitles };
        const Menu current = m_menu;
        int at = 0;
        for (int i = 0; i < 3; i++)
            if (order[i] == current) at = i;

        // Skip a list with nothing in it rather than showing an empty sheet.
        // menuRowCount() reads m_menu, so the candidate is set to ask and put
        // back if it turns out to be empty.
        for (int tries = 0; tries < 3; tries++) {
            at = (at + step + 3) % 3;
            m_menu = order[at];
            if (menuRowCount() > 0) {
                openMenu(order[at]);
                return;
            }
        }
        m_menu = current;
    }

    void applyMenuChoice(App& app)
    {
        const int count = menuRowCount();
        if (m_menuIndex < 0 || m_menuIndex >= count) return;
        m_controlsTimer = kControlsTimeout;

        switch (m_menu) {
            case Menu::Quality:
                m_menu = Menu::None;
                switchQuality(app, m_qualities[static_cast<size_t>(m_menuIndex)]);
                break;
            case Menu::Audio:
                m_menu = Menu::None;
                if (m_menuIndex < static_cast<int>(m_audioCache.size()))
                    m_player.selectAudioTrack(m_audioCache[static_cast<size_t>(m_menuIndex)].index);
                break;
            case Menu::Subtitles:
                m_menu = Menu::None;
                selectSubtitle(app, m_menuIndex - 1);
                break;
            default:
                m_menu = Menu::None;
                break;
        }
    }

    // -1 turns them off. The VTT is fetched once and kept, so toggling back
    // to a track already seen costs nothing.
    void selectSubtitle(App& app, int index)
    {
        if (index < 0) {
            m_subtitleTrack = -1;
            m_subtitles.clear();
            return;
        }
        if (index >= static_cast<int>(m_subtitleTracks.size())) return;
        if (index == m_subtitleTrack && !m_subtitles.empty()) return;

        m_subtitleTrack = index;
        m_subtitles.clear();
        m_subtitlesLoading = true;

        auto live = m_live;
        auto* tasks = &app.tasks();
        App* appPtr = &app;
        const int fileId = m_request.mediaFileId;
        const player::SubtitleTrack track = m_subtitleTracks[static_cast<size_t>(index)];
        tasks->post([this, live, tasks, appPtr, fileId, track, index] {
            const std::string url = appPtr->client().subtitleUrl(fileId, track);
            std::string body;
            if (!url.empty()) {
                net::Request req;
                req.url = url;
                const net::Response res = net::perform(req);
                if (res.ok()) body = res.body;
                else FLIKS_LOG("subs: fetch failed, HTTP %d", res.status);
            }
            tasks->postToMain([this, live, body, index] {
                if (!live->alive) return;
                m_subtitlesLoading = false;
                // A second selection may have landed while this was in
                // flight; the newer one owns the slot.
                if (index != m_subtitleTrack) return;
                if (body.empty() || !m_subtitles.parse(body)) {
                    m_subtitles.clear();
                    m_subtitleTrack = -1;
                }
            });
        });
    }

    // Reopening is the only way across: a rung change means a different
    // manifest, so the session is torn down and rebuilt at the same position.
    // The choice is stored as the preference too, since picking a rung here is
    // as much a statement of intent as picking one in Settings.
    void switchQuality(App& app, const api::QualityRung& rung)
    {
        if (rung.id == m_quality) return;
        const double resumeAt = m_player.position();
        FLIKS_LOG("player: switching to %s at %.1fs", api::qualityDescription(rung).c_str(),
                  resumeAt);

        player::Settings s = player::settings();
        s.quality = rung.id;
        player::setSettings(s);

        reportProgress(app, "paused");
        m_player.close();
        if (m_frame) {
            av_frame_free(&m_frame);
            m_frame = nullptr;
        }
        m_video.clear(app.renderer());
        stopSession(app);
        m_sessionId.clear();
        m_reportedFailure = false;
        m_status = "Switching quality…";
        resolveStream(app, resumeAt);
    }

    bool controlsVisible() const
    {
        // An open menu keeps them up: its dismiss target is a rect the
        // control bar lays out, and letting that go stale while the sheet is
        // still on screen would leave a tap hitting nothing.
        return m_menu != Menu::None || m_controlsTimer > 0.0f || m_player.paused() ||
               m_player.state() == player::State::Opening;
    }

    void drawLoading(App& app, gfx::Rect screen)
    {
        ui::Context& ctx = app.ui();
        ui::spinner(ctx, gfx::Rect{ screen.cx() - 22, screen.cy() - 22, 44, 44 }, theme::White,
                    ctx.time);
        gfx::TextStyle ts;
        ts.size = 16.0f;
        ts.color = theme::White.withAlpha(0.7f);
        ctx.text->drawAligned(m_status.empty() ? "Preparing the stream…" : m_status,
                              gfx::Rect{ screen.x, screen.cy() + 36, screen.w, 22 }, ts,
                              ui::Align::Center);
    }

    void drawError(App& app, gfx::Rect screen)
    {
        ui::Context& ctx = app.ui();
        ctx.icons->draw(*ctx.r, ui::Icon::CircleX,
                        gfx::Rect{ screen.cx() - 22, screen.cy() - 44, 44, 44 }, theme::Error);
        gfx::TextStyle ts;
        ts.size = 18.0f;
        ts.color = theme::White;
        ctx.text->drawAligned(m_player.error().empty() ? "Playback failed" : m_player.error(),
                              gfx::Rect{ screen.x + 40, screen.cy() + 12, screen.w - 80, 24 }, ts,
                              ui::Align::Center);
        gfx::TextStyle hint;
        hint.size = 14.0f;
        hint.color = theme::White.withAlpha(0.6f);
        ctx.text->drawAligned("Press B to go back",
                              gfx::Rect{ screen.x, screen.cy() + 44, screen.w, 20 }, hint,
                              ui::Align::Center);
    }

    void drawControls(App& app, gfx::Rect screen)
    {
        ui::Context& ctx = app.ui();
        const float pad = theme::metric::PagePad;

        // Scrims top and bottom so white type stays legible over any frame,
        // approximated as three bands since the renderer has no gradient.
        for (int i = 0; i < 5; i++) {
            const float t = static_cast<float>(i) / 5.0f;
            const float h = 34.0f;
            ctx.r->fillRect(gfx::Rect{ 0, screen.bottom() - (i + 1) * h, screen.w, h },
                            theme::Black.withAlpha(0.10f + 0.45f * t), 0.0f);
            ctx.r->fillRect(gfx::Rect{ 0, i * h * 0.6f, screen.w, h * 0.6f },
                            theme::Black.withAlpha(0.35f - 0.06f * i), 0.0f);
        }

        gfx::TextStyle title;
        title.size = 26.0f;
        title.bold = true;
        title.color = theme::White;
        ctx.text->drawEllipsized(m_request.title, gfx::Rect{ pad, 18, screen.w - pad * 2, 32 },
                                 title);
        if (!m_request.subtitle.empty()) {
            gfx::TextStyle sub;
            sub.size = 16.0f;
            sub.color = theme::White.withAlpha(0.75f);
            ctx.text->drawEllipsized(m_request.subtitle,
                                     gfx::Rect{ pad, 50, screen.w - pad * 2, 22 }, sub);
        }

        const double position = displayPosition();
        const double duration = effectiveDuration();

        const float barY = screen.bottom() - 96.0f;
        const gfx::Rect track{ pad, barY, screen.w - pad * 2, 6.0f };
        m_hitTrack = track;
        ctx.r->fillRect(track, theme::White.withAlpha(0.28f), 3.0f);
        if (duration > 0) {
            const float t = static_cast<float>(std::min(1.0, std::max(0.0, position / duration)));
            gfx::Rect fill = track;
            fill.w = track.w * t;
            ctx.r->fillRect(fill, theme::Primary, 3.0f);
            // The handle grows while scrubbing: at arm's length on a
            // handheld a 16px dot under a fingertip is invisible.
            const float knob = (m_scrubbing || m_pendingSeek >= 0.0) ? 22.0f : 16.0f;
            ctx.r->fillRect(gfx::Rect{ track.x + track.w * t - knob * 0.5f,
                                       track.cy() - knob * 0.5f, knob, knob },
                            theme::Primary, knob * 0.5f);

            // Where a pending seek will land, against where playback is now.
            if (m_pendingSeek >= 0.0) {
                const std::string preview = ui::formatTime(m_pendingSeek);
                const float w = ctx.text->measure(preview, 15.0f, true) + 20.0f;
                const float x = std::min(std::max(track.x, track.x + track.w * t - w * 0.5f),
                                         track.right() - w);
                const gfx::Rect chip{ x, track.y - 38.0f, w, 26.0f };
                ctx.r->fillRect(chip, theme::Black.withAlpha(0.8f), 6.0f);
                gfx::TextStyle ts;
                ts.size = 15.0f;
                ts.bold = true;
                ts.color = theme::White;
                ctx.text->drawAligned(preview, chip, ts, ui::Align::Center);
            }
        }

        gfx::TextStyle time;
        time.size = 15.0f;
        time.color = theme::White.withAlpha(0.85f);
        ctx.text->draw(ui::formatTime(position), pad, barY + 16.0f, time);
        if (duration > 0) {
            const std::string total = ui::formatTime(duration);
            ctx.text->draw(total, track.right() - ctx.text->measure(total, 15.0f, false),
                           barY + 16.0f, time);
        }

        // Real buttons now, not hints: on a handheld they are the interface,
        // and the button letter stays on each one so the controller path is
        // no harder to read than it was.
        const float row = screen.bottom() - 52.0f;
        const float size = 38.0f;
        float bx = pad;
        auto button = [&](ui::Icon icon, const char* key, bool enabled) {
            const gfx::Rect r{ bx, row - size * 0.5f, size, size };
            ctx.r->fillRect(r, theme::White.withAlpha(enabled ? 0.14f : 0.06f), size * 0.5f);
            ctx.icons->draw(*ctx.r, icon, gfx::Rect{ r.cx() - 9, r.cy() - 9, 18, 18 },
                            theme::White.withAlpha(enabled ? 0.92f : 0.35f));
            if (key && *key) {
                gfx::TextStyle ts;
                ts.size = 11.0f;
                ts.bold = true;
                ts.color = theme::White.withAlpha(0.5f);
                ctx.text->drawAligned(key, gfx::Rect{ r.x, r.bottom() + 2, r.w, 12 }, ts,
                                      ui::Align::Center);
            }
            bx += size + 10.0f;
            return r;
        };

        m_hitBack10 = button(ui::Icon::SkipBack, "←", true);
        m_hitPlay = button(m_player.paused() ? ui::Icon::Play : ui::Icon::Pause, "A", true);
        m_hitFwd10 = button(ui::Icon::SkipForward, "→", true);

        // Track pickers sit at the other end, away from the transport.
        bx = screen.right() - pad - size;
        m_hitClose = button(ui::Icon::X, "B", true);
        bx = m_hitClose.x - size - 10.0f;
        m_hitSubs = button(ui::Icon::Menu, nullptr, !m_subtitleTracks.empty());
        bx = m_hitSubs.x - size - 10.0f;
        m_hitAudio = button(ui::Icon::Tv, nullptr, m_audioCache.size() > 1);
        bx = m_hitAudio.x - size - 10.0f;
        m_hitQuality = button(ui::Icon::Settings, "Y", !m_qualities.empty());

        // What the rung actually is, since the menu is now one press away and
        // the answer should not require opening it.
        if (!m_quality.empty()) {
            gfx::TextStyle ts;
            ts.size = 13.0f;
            ts.color = theme::White.withAlpha(0.55f);
            std::string label = m_quality;
            for (const api::QualityRung& rung : m_qualities)
                if (rung.id == m_quality) label = api::qualityName(rung);
            if (m_subtitleTrack >= 0 &&
                m_subtitleTrack < static_cast<int>(m_subtitleTracks.size()))
                label += "  ·  " + m_subtitleTracks[static_cast<size_t>(m_subtitleTrack)].label();
            ctx.text->drawAligned(label,
                                  gfx::Rect{ pad, row - size * 0.5f - 24.0f,
                                             screen.w - pad * 2, 16.0f },
                                  ts, ui::Align::Right);
        }
    }

    // Subtitles sit above the control bar when it is up, so the two never
    // overlap — the same lift the web client applies on a TV.
    void drawSubtitles(App& app, gfx::Rect screen)
    {
        const player::Cue* cue = m_subtitles.at(m_player.position());
        if (!cue || cue->lines.empty()) return;

        ui::Context& ctx = app.ui();
        const float size = 22.0f;
        const float lineH = ctx.text->lineHeight(size) + 2.0f;
        const float bottom = screen.bottom() - (controlsVisible() ? 126.0f : 42.0f);
        float y = bottom - lineH * static_cast<float>(cue->lines.size());

        for (const std::string& line : cue->lines) {
            const float w = ctx.text->measure(line, size, true);
            const gfx::Rect box{ screen.cx() - w * 0.5f - 12.0f, y - 2.0f, w + 24.0f, lineH };
            // A plate rather than an outline: the renderer has no text
            // stroking, and white on white is unreadable without one.
            ctx.r->fillRect(box, theme::Black.withAlpha(0.55f), 4.0f);
            gfx::TextStyle ts;
            ts.size = size;
            ts.bold = true;
            ts.color = theme::White;
            ctx.text->drawAligned(line, gfx::Rect{ screen.x, y, screen.w, lineH }, ts,
                                  ui::Align::Center);
            y += lineH;
        }
    }

    void drawStats(App& app, gfx::Rect screen)
    {
        ui::Context& ctx = app.ui();
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%dx%d  ·  queue %d  ·  dropped %d  ·  %s",
                      m_player.videoWidth(), m_player.videoHeight(), m_player.queuedFrames(),
                      m_player.droppedFrames(), m_playMethod.c_str());
        const gfx::Rect box{ screen.right() - 380.0f, 16.0f, 364.0f, 28.0f };
        ctx.r->fillRect(box, theme::Black.withAlpha(0.6f), 6.0f);
        gfx::TextStyle ts;
        ts.size = 13.0f;
        ts.color = theme::White;
        ctx.text->drawEllipsized(buf,
                                 gfx::Rect{ box.x + 10, box.cy() - ctx.text->lineHeight(13.0f) * 0.5f,
                                            box.w - 20, ctx.text->lineHeight(13.0f) },
                                 ts);
    }

    // One sheet, three lists. Rows carry a label and an optional detail on
    // the right — bitrate for a rung, codec and channels for a track — which
    // is the same shape in every case and the reason they share a drawer.
    struct MenuRow {
        std::string label;
        std::string detail;
        bool current = false;
    };

    std::vector<MenuRow> menuRows() const
    {
        std::vector<MenuRow> rows;
        switch (m_menu) {
            case Menu::Quality:
                for (const api::QualityRung& rung : m_qualities) {
                    MenuRow row;
                    row.label = api::qualityName(rung);
                    if (rung.bitrateBps > 0) {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "%.2f Mbit/s", rung.bitrateBps / 1.0e6);
                        row.detail = buf;
                    }
                    row.current = rung.id == m_quality;
                    rows.push_back(std::move(row));
                }
                break;
            case Menu::Audio:
                for (const player::AudioTrack& track : m_audioCache) {
                    MenuRow row;
                    row.label = track.label();
                    row.current = track.index == m_player.currentAudioTrack();
                    rows.push_back(std::move(row));
                }
                break;
            case Menu::Subtitles: {
                MenuRow off;
                off.label = "Off";
                off.current = m_subtitleTrack < 0;
                rows.push_back(std::move(off));
                for (size_t i = 0; i < m_subtitleTracks.size(); i++) {
                    MenuRow row;
                    row.label = m_subtitleTracks[i].label();
                    row.current = static_cast<int>(i) == m_subtitleTrack;
                    if (row.current && m_subtitlesLoading) row.detail = "loading…";
                    rows.push_back(std::move(row));
                }
                break;
            }
            default:
                break;
        }
        return rows;
    }

    void drawMenu(App& app, gfx::Rect screen)
    {
        ui::Context& ctx = app.ui();
        const std::vector<MenuRow> rows = menuRows();
        const float rowH = 40.0f;
        const float w = 340.0f;
        const float headH = 52.0f;
        const float h = rowH * static_cast<float>(std::max<size_t>(rows.size(), 1)) + headH + 12.0f;
        const gfx::Rect panel{ screen.right() - w - 28.0f, screen.cy() - h * 0.5f, w, h };

        ctx.r->fillRect(screen, theme::Black.withAlpha(0.45f), 0.0f);
        ctx.r->fillRect(panel, theme::Base100.withAlpha(0.97f), theme::metric::RadiusBox);

        // Tabs, because Left and Right move between the lists and that has to
        // be visible or nobody will find the other two.
        const struct {
            Menu menu;
            const char* name;
        } tabs[3] = { { Menu::Quality, "Quality" },
                      { Menu::Audio, "Audio" },
                      { Menu::Subtitles, "Subtitles" } };
        float tx = panel.x + 14.0f;
        for (const auto& tab : tabs) {
            const bool on = tab.menu == m_menu;
            const float tw = ctx.text->measure(tab.name, 14.0f, on) + 18.0f;
            const gfx::Rect chip{ tx, panel.y + 12.0f, tw, 26.0f };
            if (on) ctx.r->fillRect(chip, theme::Primary, 13.0f);
            gfx::TextStyle ts;
            ts.size = 14.0f;
            ts.bold = on;
            ts.color = on ? theme::PrimaryContent : theme::BaseContent.withAlpha(0.5f);
            ctx.text->drawAligned(tab.name, chip, ts, ui::Align::Center);
            tx += tw + 6.0f;
        }

        m_menuRowH = rowH;
        m_hitMenuRows = gfx::Rect{ panel.x + 10.0f, panel.y + headH, panel.w - 20.0f,
                                   rowH * static_cast<float>(rows.size()) };

        float y = panel.y + headH;
        for (size_t i = 0; i < rows.size(); i++) {
            const MenuRow& row = rows[i];
            const bool focused = static_cast<int>(i) == m_menuIndex;
            const gfx::Rect r{ panel.x + 10.0f, y, panel.w - 20.0f, rowH - 4.0f };
            if (focused) ctx.r->fillRect(r, theme::Primary, theme::metric::RadiusField);

            gfx::TextStyle name;
            name.size = 15.0f;
            name.color = focused ? theme::PrimaryContent : theme::BaseContent;
            ctx.text->drawEllipsized(row.label,
                                     gfx::Rect{ r.x + 12, r.cy() - ctx.text->lineHeight(15.0f) * 0.5f,
                                                r.w - 120.0f, ctx.text->lineHeight(15.0f) },
                                     name);

            if (!row.detail.empty()) {
                gfx::TextStyle detail;
                detail.size = 13.0f;
                detail.color =
                    (focused ? theme::PrimaryContent : theme::BaseContent).withAlpha(0.65f);
                ctx.text->drawAligned(row.detail,
                                      gfx::Rect{ r.x, r.cy() - ctx.text->lineHeight(13.0f) * 0.5f,
                                                 r.w - 34.0f, ctx.text->lineHeight(13.0f) },
                                      detail, ui::Align::Right);
            }
            if (row.current)
                ctx.icons->draw(*ctx.r, ui::Icon::Check,
                                gfx::Rect{ r.right() - 26.0f, r.cy() - 8.0f, 16, 16 },
                                focused ? theme::PrimaryContent : theme::Primary);
            y += rowH;
        }

        if (rows.empty()) {
            gfx::TextStyle ts;
            ts.size = 14.0f;
            ts.color = theme::BaseContent.withAlpha(0.5f);
            ctx.text->drawAligned("Nothing to choose from",
                                  gfx::Rect{ panel.x, panel.y + headH + 8.0f, panel.w, 20.0f }, ts,
                                  ui::Align::Center);
        }
    }

    void resolveStream(App& app, double startAt)
    {
        auto live = m_live;
        auto* tasks = &app.tasks();
        App* appPtr = &app;
        const PlayRequest request = m_request;
        // config.txt still wins, expressed as a height; otherwise the rung the
        // user picked, which may be an eco one the height alone cannot name.
        const int heightOverride = util::configInt("maxHeight", 0);
        const std::string quality =
            heightOverride > 0 ? std::to_string(heightOverride) + "p" : player::settings().quality;
        const int maxHeight = player::qualityHeight(quality);
        const int64_t maxBandwidth = player::qualityBitrate(quality);

        m_status = "Asking the server…";
        tasks->post(
            [this, live, tasks, appPtr, request, quality, maxHeight, maxBandwidth, startAt] {
                api::PlaybackInfo info;
                std::string error;
                bool ok = appPtr->client().fetchPlaybackInfo(
                    request.mediaFileId, static_cast<int>(startAt), quality, info, error);

                // The server lists its whole ladder in the master playlist and
                // ffmpeg would take the top rung, so the variant is chosen here
                // and ffmpeg is handed that playlist instead of the master.
                if (ok && info.playUrl.find(".m3u8") != std::string::npos) {
                    net::Request master;
                    master.url = info.playUrl;
                    const net::Response body = net::perform(master);
                    std::string variantUrl;
                    int height = 0;
                    if (body.ok() &&
                        player::selectHlsVariant(info.playUrl, body.body, maxHeight, maxBandwidth,
                                                 variantUrl, height)) {
                        info.playUrl = variantUrl;
                        if (height > 0) info.height = height;
                    }
                }

                tasks->postToMain([this, live, ok, info, error, startAt] {
                    if (!live->alive) return;
                    if (!ok) {
                        FLIKS_LOG("player: playback-info failed: %s", error.c_str());
                        m_status = error.empty() ? "The server refused the stream" : error;
                        return;
                    }
                    FLIKS_LOG("player: %s, container=%s, %dx%d, url=%s",
                              info.playMethod.c_str(), info.container.c_str(), info.width,
                              info.height, util::loggableUrl(info.playUrl).c_str());
                    m_sessionId = info.sessionId;
                    m_playMethod = info.playMethod;
                    m_declaredDuration = info.durationSeconds;
                    m_qualities = info.qualities;
                    m_quality = info.quality;
                    m_status = "Buffering…";
                    // `startAt` only tells the backend where to pre-spawn
                    // ffmpeg; the manifest still covers the whole file, so
                    // the seek to the resume point happens here either way.
                    m_player.open(info.playUrl, startAt);
                });
            },
            util::TaskQueue::Priority::High);
    }

    void reportProgress(App& app, const char* state)
    {
        if (m_request.mediaId <= 0 || m_player.state() == player::State::Idle) return;
        const double position = m_player.position();
        const double duration = m_player.duration() > 0 ? m_player.duration() : m_declaredDuration;
        if (duration <= 0) return;

        auto* tasks = &app.tasks();
        App* appPtr = &app;
        const PlayRequest request = m_request;
        const std::string session = m_sessionId;
        const std::string stateName = state;
        tasks->post([appPtr, request, position, duration, session, stateName] {
            appPtr->client().reportProgress(request.mediaId, request.mediaFileId, request.episodeId,
                                            position, duration, session, stateName);
        });
    }

    void stopSession(App& app)
    {
        if (m_sessionId.empty()) return;
        auto* tasks = &app.tasks();
        App* appPtr = &app;
        const std::string session = m_sessionId;
        tasks->post([appPtr, session] { appPtr->client().stopSession(session); });
    }

    std::shared_ptr<Liveness> m_live;
    PlayRequest m_request;
    player::Player m_player;
    player::VideoRenderer m_video;
    AVFrame* m_frame = nullptr;
    std::string m_sessionId;
    std::string m_playMethod;
    std::string m_status;
    double m_declaredDuration = 0;
    float m_controlsTimer = kControlsTimeout;
    float m_heartbeat = 0;
    unsigned m_traceFrames = 0;
    bool m_reportedFailure = false;
    std::vector<api::QualityRung> m_qualities;
    std::string m_quality;

    Menu m_menu = Menu::None;
    int m_menuIndex = 0;

    int m_lastTapZone = -1;
    float m_lastTapTime = -10.0f;

    std::vector<player::AudioTrack> m_audioCache;
    std::vector<player::SubtitleTrack> m_subtitleTracks;
    int m_subtitleTrack = -1;   // index into m_subtitleTracks, -1 for off
    player::Subtitles m_subtitles;
    bool m_subtitlesLoading = false;

    // Seeks are accumulated and committed once the user stops pressing: on a
    // fed stream each one rebuilds the session, so five taps of Right must
    // not mean five rebuilds.
    double m_pendingSeek = -1.0;
    float m_seekCommit = 0.0f;
    bool m_scrubbing = false;

    // Hit rects, filled while drawing and read by the next frame's input.
    // Laying out twice to answer a tap would be the alternative.
    gfx::Rect m_hitTrack{};
    gfx::Rect m_hitPlay{};
    gfx::Rect m_hitBack10{};
    gfx::Rect m_hitFwd10{};
    gfx::Rect m_hitQuality{};
    gfx::Rect m_hitAudio{};
    gfx::Rect m_hitSubs{};
    gfx::Rect m_hitClose{};
    gfx::Rect m_hitMenuRows{};
    float m_menuRowH = 0.0f;
};

} // namespace

std::unique_ptr<Screen> makePlayerScreen(PlayRequest request)
{
    return std::make_unique<PlayerScreen>(std::move(request));
}

} // namespace app
