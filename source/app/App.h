#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app/Screen.h"
#include "app/Theme.h"
#include "gfx/ImageStore.h"
#include "gfx/Renderer.h"
#include "gfx/Text.h"
#include "net/Api.h"
#include "ui/Focus.h"
#include "ui/Input.h"
#include "ui/Ui.h"
#include "util/TaskQueue.h"

namespace app {

struct Toast {
    std::string message;
    float remaining = 0;
    bool error = false;
};

class App {
public:
    bool init();
    void run();
    void shutdown();

    ui::Context& ui() { return m_ui; }
    gfx::Renderer& renderer() { return m_renderer; }
    gfx::TextRenderer& text() { return m_text; }
    gfx::ImageStore& images() { return m_images; }
    ui::FocusManager& focus() { return m_focus; }
    ui::Input& input() { return m_input; }
    api::Client& client() { return m_client; }
    util::TaskQueue& tasks() { return m_tasks; }

    // Content rect below the top bar, inside the page padding.
    gfx::Rect contentRect() const;
    gfx::TexId logo() const { return m_logoTex; }
    float pagePad() const { return theme::metric::PagePad; }

    void push(std::unique_ptr<Screen> screen);
    void replace(std::unique_ptr<Screen> screen);
    void pop();
    Screen* top() const { return m_screens.empty() ? nullptr : m_screens.back().get(); }

    void toast(const std::string& message, bool error = false);
    void setLibraries(std::vector<api::Library> libs) { m_libraries = std::move(libs); }
    const std::vector<api::Library>& libraries() const { return m_libraries; }

    // Bumped on every navigation; async results captured with a stale token
    // are dropped instead of landing on a screen the user already left.
    uint64_t navToken() const { return m_navToken; }

    void requestQuit() { m_running = false; }
    bool drawerOpen() const { return m_drawerOpen; }
    void openDrawer();
    void closeDrawer();

private:
    void drawBackground();
    void drawTopBar();
    void drawDrawer();
    void drawToasts();
    void handleShellInput();
    // Turns a tap into the focus change and button press the screens already
    // handle, so touch needed no second path through every screen.
    void handleTouchShell();
    // Returns true when the prompt owns the frame's input, which is what
    // keeps the screen underneath from also acting on it.
    bool handleQuitPrompt();
    void drawQuitPrompt();
    void applyPendingScreens();
    void setBackground(const std::string& url);

    gfx::Renderer m_renderer;
    gfx::TextRenderer m_text;
    gfx::ImageStore m_images;
    ui::IconSet m_icons;
    ui::FocusManager m_focus;
    ui::Input m_input;
    api::Client m_client;
    util::TaskQueue m_tasks;
    ui::Context m_ui;

    std::vector<std::unique_ptr<Screen>> m_screens;
    std::vector<std::unique_ptr<Screen>> m_pendingPush;
    int m_pendingPop = 0;
    bool m_pendingReplace = false;

    std::vector<api::Library> m_libraries;
    std::vector<Toast> m_toasts;

    // Two layers so a fanart change cross-fades the way `app-background` does.
    std::string m_bgCurrent;
    std::string m_bgPrevious;
    float m_bgFade = 1.0f;

    gfx::TexId m_logoTex = gfx::TexInvalid;
    bool m_drawerOpen = false;
    // Filled while drawing, read by the next frame's touch handling.
    std::vector<gfx::Rect> m_drawerRows;
    gfx::Rect m_drawerPanel{};
    gfx::Rect m_backRect{};

    // Quitting has to be confirmable from anywhere, not only the home screen,
    // so the prompt lives in the shell rather than in a screen.
    bool m_quitPrompt = false;
    bool m_quitConfirm = false;   // which button is selected
    gfx::Rect m_quitYes{};
    gfx::Rect m_quitNo{};
    float m_drawerOffset = 0.0f;
    int m_drawerIndex = 0;
    uint64_t m_navToken = 1;
    uint64_t m_frameCounter = 0;
    bool m_running = true;
    double m_time = 0.0;
};

} // namespace app
