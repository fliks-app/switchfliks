#pragma once

#include <switch.h>

#include "gfx/Color.h"
#include "ui/Focus.h"

namespace ui {

// D-pad, both sticks and the face buttons, with auto-repeat on the
// directional axes. The web client paces held-key navigation at 300ms because
// a TV WebView's smooth scroll judders under faster input; this renderer
// animates at 60fps, so the repeat is tuned for a gamepad instead.
class Input {
public:
    void init();
    void update(float dt);

    bool pressed(uint64_t buttons) const { return (m_down & buttons) != 0; }
    bool held(uint64_t buttons) const { return (m_held & buttons) != 0; }
    bool released(uint64_t buttons) const { return (m_up & buttons) != 0; }

    // True on the initial press and again on each repeat tick.
    bool navigate(Dir dir) const { return m_navigate[static_cast<int>(dir)]; }
    bool anyNavigate() const;

    float stickX() const { return m_stickX; }
    float stickY() const { return m_stickY; }

    // Touch, in virtual-canvas coordinates like everything else the screens
    // deal in — the panel reports 1280x720 and the UI lays out at 960x540, so
    // raw values would miss every rect by a third. Docked has no touchscreen,
    // where `active()` simply never becomes true.
    bool touchActive() const { return m_touchActive; }
    bool touchDown() const { return m_touchDown; }    // began this frame
    bool touchUp() const { return m_touchUp; }        // ended this frame
    float touchX() const { return m_touchX; }
    float touchY() const { return m_touchY; }
    // Movement since the previous frame; zero on the frame of the initial
    // press, so a drag never jumps by wherever the finger landed.
    float touchDX() const { return m_touchDX; }
    float touchDY() const { return m_touchDY; }
    float touchStartX() const { return m_touchStartX; }
    float touchStartY() const { return m_touchStartY; }

    // A press that began inside `r` — the anchor for a drag, so a scrubber
    // keeps following the finger past its own edge.
    bool pressedIn(gfx::Rect r) const { return m_touchDown && r.contains(m_touchX, m_touchY); }
    // A release inside `r` that never wandered far from where it began. The
    // slop is what separates a tap from the start of a drag or a scroll.
    bool tapped(gfx::Rect r) const
    {
        return m_touchUp && m_tapCandidate && r.contains(m_touchX, m_touchY) &&
               r.contains(m_touchStartX, m_touchStartY);
    }
    // True while the finger is the thing steering. The focus cursor belongs
    // to the pad, and a list that keeps scrolling itself back to the cursor
    // is unusable under touch — so reveal-on-focus is skipped while this
    // holds. Any button or stick input hands control back.
    bool touchDriven() const { return m_touchDriven; }

    // Makes a tap indistinguishable from the button press it stands for, so
    // every screen's existing `pressed(A)` handling works under touch with no
    // second code path to keep in step. Cleared by the next update().
    void injectPress(uint64_t buttons) { m_down |= buttons; }

    // Set once, when the renderer knows its viewport.
    void setScale(float scale) { m_scale = scale > 0.0f ? scale : 1.0f; }

private:
    static constexpr float kRepeatDelay = 0.35f;
    static constexpr float kRepeatRate = 0.11f;
    static constexpr int32_t kStickDeadzone = 12000;

    void updateTouch();

    PadState m_pad{};
    uint64_t m_down = 0, m_held = 0, m_up = 0;
    bool m_navigate[4] = { false, false, false, false };
    bool m_dirHeld[4] = { false, false, false, false };
    float m_dirTimer[4] = { 0, 0, 0, 0 };
    float m_stickX = 0, m_stickY = 0;

    // Past this much travel a press is a drag, not a tap.
    static constexpr float kTapSlop = 12.0f;

    float m_scale = 1.0f;
    bool m_touchDriven = false;
    bool m_touchActive = false;
    bool m_touchDown = false;
    bool m_touchUp = false;
    bool m_tapCandidate = false;
    float m_touchX = 0, m_touchY = 0;
    float m_touchDX = 0, m_touchDY = 0;
    float m_touchStartX = 0, m_touchStartY = 0;
};

} // namespace ui
