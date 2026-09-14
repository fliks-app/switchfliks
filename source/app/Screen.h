#pragma once

#include <string>

namespace app {

class App;

class Screen {
public:
    virtual ~Screen() = default;

    virtual void onEnter(App&) {}
    virtual void onExit(App&) {}
    virtual void onResume(App&) {}   // the screen below became top again

    // Input runs against the previous frame's focus layout, then draw
    // re-registers it. Returning true from handleInput swallows the press so
    // the shell does not also act on it.
    virtual bool handleInput(App&) { return false; }
    virtual void update(App&, float dt) { (void)dt; }
    virtual void draw(App&) = 0;

    // Fanart behind the page, the way `app-background` drives it.
    virtual std::string backgroundUrl() const { return {}; }
    // Hero pages let the fanart read through the top bar.
    virtual bool heroPage() const { return false; }
    virtual bool showsChrome() const { return true; }
    virtual std::string title() const { return {}; }
};

} // namespace app
