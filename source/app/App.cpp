#include "app/App.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <malloc.h>

#include <switch.h>

#include "app/Screens.h"
#include "gfx/ImageDecode.h"
#include "util/Log.h"

namespace app {

using theme::metric::PagePad;
using theme::metric::SidebarW;
using theme::metric::TopBarH;

namespace {

gfx::TexId loadPngTexture(gfx::Renderer& renderer, const char* path)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) return gfx::TexInvalid;
    std::string bytes;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.append(buf, n);
    std::fclose(f);

    gfx::DecodedImage image;
    if (!gfx::decodeImage(bytes, 0, image) || !image.valid()) return gfx::TexInvalid;
    return renderer.createTexture(image.width, image.height, DkImageFormat_RGBA8_Unorm,
                                  image.rgba.data());
}

float approach(float current, float target, float dt, float rate)
{
    const float t = 1.0f - std::exp(-rate * dt);
    return current + (target - current) * t;
}

} // namespace

bool App::init()
{
    if (!m_renderer.init()) {
        FLIKS_LOG("app: renderer init failed");
        return false;
    }
    if (!m_text.init(m_renderer)) {
        FLIKS_LOG("app: no shared fonts (is pl initialised?)");
        return false;
    }
    u64 totalMem = 0, usedMem = 0;
    svcGetInfo(&totalMem, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&usedMem, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    FLIKS_LOG("app: applet type %d (title takeover=%d), heap %llu/%llu MiB",
              static_cast<int>(appletGetAppletType()), gfx::Renderer::titleTakeover() ? 1 : 0,
              static_cast<unsigned long long>(usedMem / (1024 * 1024)),
              static_cast<unsigned long long>(totalMem / (1024 * 1024)));

    if (!m_icons.load(m_renderer, "romfs:/icons.png")) FLIKS_LOG("app: icon atlas missing");
    m_logoTex = loadPngTexture(m_renderer, "romfs:/logo.png");
    if (m_logoTex == gfx::TexInvalid) FLIKS_LOG("app: logo missing");

    m_tasks.start(3);
    m_images.init(&m_renderer, &m_client, &m_tasks);
    m_input.init();

    m_ui.r = &m_renderer;
    m_ui.text = &m_text;
    m_ui.images = &m_images;
    m_ui.icons = &m_icons;
    m_ui.focus = &m_focus;
    m_ui.input = &m_input;
    m_ui.api = &m_client;
    m_ui.tasks = &m_tasks;

    // A stored session skips straight to the home page; anything wrong with
    // it (rotated refresh token, server moved) falls back to the setup flow,
    // which is also where a first run starts.
    if (m_client.loadSession() && m_client.hasSession()) {
        push(makeHomeScreen());
    } else {
        push(makeServerScreen());
    }
    applyPendingScreens();
    FLIKS_LOG("app: ready, server='%s', session=%d", m_client.server().c_str(),
              m_client.hasSession() ? 1 : 0);
    return true;
}

void App::shutdown()
{
    // The top screen is the only entered one, and its onExit is what reports
    // the playback position and closes the server session. Quitting used to
    // destroy it outright, so leaving mid-film lost your place.
    if (!m_screens.empty()) m_screens.back()->onExit(*this);
    m_screens.clear();
    m_tasks.stop();
    m_images.shutdown();
    m_text.shutdown();
    m_renderer.shutdown();
}

gfx::Rect App::contentRect() const
{
    const float top = TopBarH;
    return gfx::Rect{ PagePad, top, m_renderer.width() - PagePad * 2.0f,
                      m_renderer.height() - top };
}

void App::push(std::unique_ptr<Screen> screen)
{
    m_pendingPush.push_back(std::move(screen));
    m_navToken++;
}

void App::replace(std::unique_ptr<Screen> screen)
{
    m_pendingReplace = true;
    m_pendingPush.push_back(std::move(screen));
    m_navToken++;
}

void App::pop()
{
    m_pendingPop++;
    m_navToken++;
}

void App::applyPendingScreens()
{
    if (m_pendingReplace && !m_screens.empty()) {
        m_screens.back()->onExit(*this);
        m_screens.pop_back();
        m_pendingReplace = false;
    }
    while (m_pendingPop > 0 && !m_screens.empty()) {
        m_screens.back()->onExit(*this);
        m_screens.pop_back();
        m_pendingPop--;
        if (!m_screens.empty()) m_screens.back()->onResume(*this);
    }
    m_pendingPop = 0;

    for (auto& screen : m_pendingPush) {
        m_screens.push_back(std::move(screen));
        m_screens.back()->onEnter(*this);
    }
    if (!m_pendingPush.empty()) {
        m_pendingPush.clear();
        m_focus.setCurrent(ui::FocusNone);
    }
    if (m_screens.empty()) m_running = false;
}

void App::toast(const std::string& message, bool error)
{
    m_toasts.push_back(Toast{ message, 4.0f, error });
    if (m_toasts.size() > 3) m_toasts.erase(m_toasts.begin());
}

void App::openDrawer()
{
    if (m_drawerOpen) return;
    m_drawerOpen = true;
    m_drawerIndex = 0;
}

void App::closeDrawer() { m_drawerOpen = false; }

void App::setBackground(const std::string& url)
{
    if (url == m_bgCurrent) return;
    m_bgPrevious = m_bgCurrent;
    m_bgCurrent = url;
    m_bgFade = 0.0f;
}

void App::run()
{
    uint64_t previousTick = armGetSystemTick();

    while (m_running && appletMainLoop()) {
        const uint64_t tick = armGetSystemTick();
        float dt = static_cast<float>(armTicksToNs(tick - previousTick)) / 1.0e9f;
        previousTick = tick;
        dt = std::min(dt, 0.1f);
        m_time += dt;
        m_ui.dt = dt;
        m_ui.time = m_time;

        // Docking changes the viewport, and touch coordinates are reported in
        // panel pixels whatever the UI lays out in.
        m_input.setScale(m_renderer.scale());
        m_input.update(dt);
        m_tasks.drainMain();
        applyPendingScreens();
        if (!m_running || m_screens.empty()) break;

        Screen* screen = m_screens.back().get();
        setBackground(screen->backgroundUrl());
        m_bgFade = std::min(1.0f, m_bgFade + dt * 2.5f);
        m_drawerOffset = approach(m_drawerOffset, m_drawerOpen ? 1.0f : 0.0f, dt, 18.0f);

        screen->update(*this, dt);
        // The quit prompt is modal: it takes the frame's input before anything
        // else sees it, which is also how Plus reaches the shell from the
        // player — that screen answers every button itself and nothing would
        // otherwise get past it.
        if (!handleQuitPrompt()) {
            // Screens without chrome — the player — own their touch handling
            // entirely; everywhere else the shell turns a tap into the focus
            // change and button press the screens already understand.
            if (screen->showsChrome()) handleTouchShell();
            if (!screen->handleInput(*this)) handleShellInput();
        }

        for (auto& t : m_toasts) t.remaining -= dt;
        m_toasts.erase(std::remove_if(m_toasts.begin(), m_toasts.end(),
                                      [](const Toast& t) { return t.remaining <= 0; }),
                       m_toasts.end());

        m_renderer.syncResolution();
        m_images.tick();

        if (!m_renderer.beginFrame()) {
            // The compositor took the window back (sleep, a library applet,
            // a dock transition). Rebuild and try again next tick.
            m_renderer.syncResolution(true);
            continue;
        }
        m_renderer.clear(theme::Base100);
        m_focus.beginFrame();

        drawBackground();
        screen->draw(*this);
        if (screen->showsChrome()) {
            drawTopBar();
            drawDrawer();
        }
        drawToasts();
        drawQuitPrompt();

        m_focus.endFrame();
        m_renderer.endFrame();

        // Bug hunting: the swapchain has died at wildly different times, which
        // points at something accumulating rather than a code path.
        if (++m_frameCounter % 240 == 0) {
            const struct mallinfo mi = mallinfo();
            u64 used = 0;
            svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
            FLIKS_LOG("health f=%llu heap=%lluKiB free=%lluKiB used=%lluMiB tex=%zu staging=%zu",
                      static_cast<unsigned long long>(m_frameCounter),
                      static_cast<unsigned long long>(mi.arena / 1024),
                      static_cast<unsigned long long>(mi.fordblks / 1024),
                      static_cast<unsigned long long>(used / (1024 * 1024)),
                      m_renderer.liveTextures(), m_renderer.stagingInFlight());
        }
    }
}

namespace {
// A swipe that begins this close to the left edge and travels this far opens
// the drawer; the same distance leftwards closes it.
constexpr float kEdgeSwipeZone = 28.0f;
constexpr float kEdgeSwipeDistance = 44.0f;
} // namespace

void App::handleTouchShell()
{
    ui::Input& in = m_input;
    if (!in.touchUp()) return;

    // The drawer has no button anywhere — the D-pad reaches it by pressing
    // Left at the leftmost column, and the touch equivalent of that is an
    // edge swipe. Adding a hamburger would have put chrome on a 10-foot
    // layout to serve the handheld one.
    const float swipe = in.touchX() - in.touchStartX();
    if (!m_drawerOpen && in.touchStartX() <= kEdgeSwipeZone && swipe >= kEdgeSwipeDistance) {
        openDrawer();
        return;
    }
    if (m_drawerOpen && swipe <= -kEdgeSwipeDistance) {
        closeDrawer();
        return;
    }

    if (m_drawerOpen) {
        for (size_t i = 0; i < m_drawerRows.size(); i++) {
            if (!in.tapped(m_drawerRows[i])) continue;
            // Selecting then injecting A reuses the drawer's own activation,
            // rather than a second copy of where each entry goes.
            m_drawerIndex = static_cast<int>(i);
            in.injectPress(HidNpadButton_A);
            return;
        }
        // Off the panel is how a drawer is dismissed everywhere.
        if (!m_drawerPanel.contains(in.touchX(), in.touchY())) closeDrawer();
        return;
    }

    if (!m_backRect.empty() && in.tapped(m_backRect)) {
        pop();
        return;
    }

    const ui::FocusId hit = m_focus.hitTest(in.touchX(), in.touchY());
    if (hit == ui::FocusNone) return;
    // The press must land on what was touched, not on whatever the D-pad
    // left focused, so focus moves first.
    if (!in.tapped(m_focus.rectOf(hit))) return;
    m_focus.setCurrent(hit);
    in.injectPress(HidNpadButton_A);
}

bool App::handleQuitPrompt()
{
    ui::Input& in = m_input;

    if (!m_quitPrompt) {
        // Opened from anywhere, including mid-playback. The screen never sees
        // this press, so nothing has to opt in.
        if (!in.pressed(HidNpadButton_Plus)) return false;
        m_quitPrompt = true;
        m_quitConfirm = false;   // No is selected, so a stray press is safe
        closeDrawer();
        return true;
    }

    if (in.touchUp()) {
        if (in.tapped(m_quitYes)) {
            m_running = false;
            return true;
        }
        // Anywhere else, the backdrop included, means no.
        m_quitPrompt = false;
        return true;
    }
    if (in.touchActive()) return true;

    if (in.navigate(ui::Dir::Left) || in.navigate(ui::Dir::Right))
        m_quitConfirm = !m_quitConfirm;
    if (in.pressed(HidNpadButton_B) || in.pressed(HidNpadButton_Plus)) {
        m_quitPrompt = false;
        return true;
    }
    if (in.pressed(HidNpadButton_A)) {
        if (m_quitConfirm) m_running = false;
        else m_quitPrompt = false;
    }
    return true;
}

void App::drawQuitPrompt()
{
    if (!m_quitPrompt) return;

    const gfx::Rect screen{ 0, 0, m_renderer.width(), m_renderer.height() };
    m_renderer.fillRect(screen, theme::Black.withAlpha(0.6f), 0.0f);

    const float w = std::min(420.0f, screen.w - PagePad * 2.0f);
    const gfx::Rect box{ screen.cx() - w * 0.5f, screen.cy() - 84.0f, w, 168.0f };
    m_renderer.fillRect(box, theme::Base100, theme::metric::RadiusBox);

    gfx::TextStyle title;
    title.size = 20.0f;
    title.bold = true;
    title.color = theme::BaseContent;
    m_text.drawAligned("Quit Fliks?", gfx::Rect{ box.x, box.y + 26.0f, box.w, 26.0f }, title,
                       ui::Align::Center);

    gfx::TextStyle body;
    body.size = 15.0f;
    body.color = theme::BaseContent.withAlpha(0.65f);
    m_text.drawAligned("Playback will stop and you will return to the home menu.",
                       gfx::Rect{ box.x + 20.0f, box.y + 58.0f, box.w - 40.0f, 22.0f }, body,
                       ui::Align::Center);

    const float bw = (box.w - 60.0f) * 0.5f;
    const float by = box.bottom() - 62.0f;
    m_quitNo = gfx::Rect{ box.x + 20.0f, by, bw, 44.0f };
    m_quitYes = gfx::Rect{ m_quitNo.right() + 20.0f, by, bw, 44.0f };

    auto choice = [&](gfx::Rect r, const char* label, bool selected, gfx::Color fill) {
        m_renderer.fillRect(r, selected ? fill : theme::Base300, theme::metric::RadiusField);
        if (selected) {
            // The same two-layer ring the cards use, so the prompt reads as
            // part of the same interface.
            m_renderer.strokeRect(r.expand(theme::metric::FocusGap), theme::BaseContent,
                                  theme::metric::RadiusField + theme::metric::FocusGap, 2.0f);
        }
        gfx::TextStyle ts;
        ts.size = 16.0f;
        ts.bold = true;
        ts.color = selected ? theme::White : theme::BaseContent.withAlpha(0.8f);
        m_text.drawAligned(label, gfx::Rect{ r.x, r.cy() - m_text.lineHeight(16.0f) * 0.5f, r.w,
                                             m_text.lineHeight(16.0f) },
                           ts, ui::Align::Center);
    };
    choice(m_quitNo, "No", !m_quitConfirm, theme::Base300);
    choice(m_quitYes, "Yes", m_quitConfirm, theme::Error);
}

void App::handleShellInput()
{
    ui::Input& in = m_input;

    if (m_drawerOpen) {
        if (in.pressed(HidNpadButton_B) || in.navigate(ui::Dir::Right)) {
            closeDrawer();
            return;
        }
        const int count = static_cast<int>(m_libraries.size()) + 3;
        if (in.navigate(ui::Dir::Down)) m_drawerIndex = std::min(m_drawerIndex + 1, count - 1);
        if (in.navigate(ui::Dir::Up)) m_drawerIndex = std::max(m_drawerIndex - 1, 0);
        if (in.pressed(HidNpadButton_A)) {
            closeDrawer();
            if (m_drawerIndex == 0) {
                while (m_screens.size() > 1) pop();
            } else if (m_drawerIndex == 1) {
                push(makeSearchScreen());
            } else if (m_drawerIndex - 2 < static_cast<int>(m_libraries.size())) {
                const api::Library& lib = m_libraries[m_drawerIndex - 2];
                push(makeLibraryScreen(lib.id, lib.name));
            } else {
                push(makeSettingsScreen());
            }
        }
        return;
    }

    // Coming back to the pad after a touch scroll: if the cursor is off
    // screen, the first press brings it back to what is visible rather than
    // scrolling the page to wherever it was left.
    if (in.anyNavigate()) {
        const gfx::Rect view = contentRect();
        const gfx::Rect cur = m_focus.currentRect();
        if ((cur.empty() || view.intersect(cur).empty()) && m_focus.focusNearest(view)) return;
    }

    if (in.navigate(ui::Dir::Left)) {
        // Left at the leftmost column is what reaches the nav on a 10-foot
        // layout — the client gets there through the same spatial step into
        // the sidebar, which has no permanent column on TV.
        const gfx::Rect before = m_focus.currentRect();
        if (!m_focus.move(ui::Dir::Left) && before.x <= PagePad + 1.0f) openDrawer();
        return;
    }
    if (in.navigate(ui::Dir::Right)) m_focus.move(ui::Dir::Right);
    if (in.navigate(ui::Dir::Up)) m_focus.move(ui::Dir::Up);
    if (in.navigate(ui::Dir::Down)) m_focus.move(ui::Dir::Down);

    if (in.pressed(HidNpadButton_B)) {
        if (m_screens.size() > 1) pop();
    }
    if (in.pressed(HidNpadButton_Minus)) openDrawer();
}

void App::drawBackground()
{
    const gfx::Rect full{ 0, 0, m_renderer.width(), m_renderer.height() };
    if (m_bgCurrent.empty() && m_bgPrevious.empty()) return;

    // `object-cover object-[50%_25%]`: the crop is biased to the upper part
    // of the fanart so faces survive it.
    auto drawLayer = [&](const std::string& url, float alpha) {
        if (url.empty() || alpha <= 0.0f) return;
        const int decodeWidth = static_cast<int>(m_renderer.fbWidth());
        const gfx::TexId tex = m_images.get(url, "medium", decodeWidth);
        if (tex == gfx::TexInvalid) return;
        gfx::Rect uv = ui::coverUv(m_renderer.textureWidth(tex), m_renderer.textureHeight(tex), full);
        if (uv.h < 1.0f) uv.y = (1.0f - uv.h) * 0.25f;
        m_renderer.drawImage(tex, full, theme::White.withAlpha(alpha), 0.0f, uv);
    };

    drawLayer(m_bgPrevious, 1.0f - m_bgFade);
    drawLayer(m_bgCurrent, m_bgFade);
    m_renderer.fillRect(full, theme::BgVeil, 0.0f);
}

void App::drawTopBar()
{
    Screen* screen = m_screens.back().get();
    const gfx::Rect bar{ 0, 0, m_renderer.width(), TopBarH };

    // `body.tv .app-navbar-veil` is transparent so the fanart reads across
    // the top; without a fanart the page colour is already behind it.
    if (m_bgCurrent.empty() && !screen->heroPage())
        m_renderer.fillRect(bar, theme::Base100, 0.0f);

    float x = PagePad;
    m_backRect = gfx::Rect{};
    if (m_screens.size() > 1) {
        m_icons.draw(m_renderer, ui::Icon::ChevronLeft,
                     gfx::Rect{ x, bar.cy() - 11, 22, 22 }, theme::BaseContent);
        // A 22px glyph is not a touch target; the tappable area is the
        // whole left end of the bar.
        m_backRect = gfx::Rect{ 0, 0, x + 44.0f, TopBarH };
        x += 32;
    }

    const std::string title = screen->title();
    gfx::TextStyle ts;
    ts.size = 20.0f;
    ts.bold = true;
    ts.color = theme::BaseContent;
    if (!title.empty())
        m_text.draw(title, x, bar.cy() - m_text.lineHeight(ts.size) * 0.5f, ts);

    const api::User user = m_client.currentUser();
    if (!user.username.empty()) {
        gfx::TextStyle name;
        name.size = 15.0f;
        name.color = theme::BaseContent.withAlpha(0.6f);
        const float w = m_text.measure(user.username, name.size, false);
        const float right = m_renderer.width() - PagePad;
        m_text.draw(user.username, right - w - 28,
                    bar.cy() - m_text.lineHeight(name.size) * 0.5f, name);
        m_icons.draw(m_renderer, ui::Icon::User, gfx::Rect{ right - 22, bar.cy() - 11, 22, 22 },
                     theme::BaseContent.withAlpha(0.6f));
    }
}

void App::drawDrawer()
{
    if (m_drawerOffset <= 0.001f) return;

    const float h = m_renderer.height();
    const gfx::Rect scrim{ 0, 0, m_renderer.width(), h };
    m_renderer.fillRect(scrim, theme::Black.withAlpha(0.5f * m_drawerOffset), 0.0f);

    const float x = -SidebarW * (1.0f - m_drawerOffset);
    const gfx::Rect panel{ x, 0, SidebarW, h };
    m_renderer.fillRect(panel, theme::Base100, 0.0f);
    m_renderer.fillRect(gfx::Rect{ panel.right() - 1, 0, 1, h },
                        theme::BaseContent.withAlpha(0.08f), 0.0f);

    float y = 16.0f;
    {
        gfx::TextStyle brand;
        brand.size = 24.0f;
        brand.bold = true;
        brand.color = theme::BaseContent;
        m_text.draw("Fliks", panel.x + 16, y, brand);
        y += 44.0f;
        m_renderer.fillRect(gfx::Rect{ panel.x + 16, y, SidebarW - 32, 1 },
                            theme::BaseContent.withAlpha(0.08f), 0.0f);
        y += 12.0f;
    }

    struct Entry {
        ui::Icon icon;
        std::string label;
    };
    std::vector<Entry> entries;
    entries.push_back(Entry{ ui::Icon::Home, "Home" });
    entries.push_back(Entry{ ui::Icon::Search, "Search" });
    for (const api::Library& lib : m_libraries)
        entries.push_back(Entry{ lib.hasSeries ? ui::Icon::Tv : ui::Icon::Clapperboard, lib.name });
    entries.push_back(Entry{ ui::Icon::Settings, "Settings" });

    m_drawerRows.clear();
    m_drawerPanel = panel;
    for (size_t i = 0; i < entries.size(); i++) {
        const gfx::Rect row{ panel.x + 12, y, SidebarW - 24, 40 };
        m_drawerRows.push_back(row);
        const bool selected = static_cast<int>(i) == m_drawerIndex;
        if (selected) {
            // `.menu-active`: a 10% tint of base-content rather than the
            // neutral fill, which reads as a hole in this dark theme.
            m_renderer.fillRect(row, theme::BaseContent.withAlpha(0.10f),
                                theme::metric::RadiusBox);
            m_renderer.strokeRect(row, theme::Primary, theme::metric::RadiusBox, 2.0f);
        }
        m_icons.draw(m_renderer, entries[i].icon, gfx::Rect{ row.x + 12, row.cy() - 10, 20, 20 },
                     theme::BaseContent.withAlpha(selected ? 1.0f : 0.75f));
        gfx::TextStyle ts;
        ts.size = theme::metric::BodyFont;
        ts.color = theme::BaseContent.withAlpha(selected ? 1.0f : 0.75f);
        m_text.drawEllipsized(entries[i].label,
                              gfx::Rect{ row.x + 42, row.cy() - m_text.lineHeight(ts.size) * 0.5f,
                                         row.w - 54, m_text.lineHeight(ts.size) },
                              ts);
        y += 44.0f;

        if (i == 1 || i + 2 == entries.size()) {
            m_renderer.fillRect(gfx::Rect{ panel.x + 16, y + 4, SidebarW - 32, 1 },
                                theme::BaseContent.withAlpha(0.08f), 0.0f);
            y += 12.0f;
        }
    }
}

void App::drawToasts()
{
    if (m_toasts.empty()) return;
    const float width = 360.0f;
    float y = m_renderer.height() - PagePad - 48.0f;

    for (auto it = m_toasts.rbegin(); it != m_toasts.rend(); ++it) {
        const float alpha = std::min(1.0f, it->remaining);
        const gfx::Rect box{ m_renderer.width() - PagePad - width, y, width, 44 };
        m_renderer.fillRect(box, (it->error ? theme::Error : theme::Base200).withAlpha(alpha),
                            theme::metric::RadiusBox);
        gfx::TextStyle ts;
        ts.size = 16.0f;
        ts.color = (it->error ? theme::Base100 : theme::BaseContent).withAlpha(alpha);
        m_text.drawEllipsized(it->message,
                              gfx::Rect{ box.x + 14, box.cy() - m_text.lineHeight(ts.size) * 0.5f,
                                         box.w - 28, m_text.lineHeight(ts.size) },
                              ts);
        y -= 52.0f;
    }
}

} // namespace app
