#include "ui/Input.h"

#include <cmath>

namespace ui {

void Input::init()
{
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&m_pad);
    hidInitializeTouchScreen();
}

void Input::update(float dt)
{
    padUpdate(&m_pad);
    m_down = padGetButtonsDown(&m_pad);
    m_held = padGetButtons(&m_pad);
    m_up = padGetButtonsUp(&m_pad);

    const HidAnalogStickState left = padGetStickPos(&m_pad, 0);
    m_stickX = static_cast<float>(left.x) / 32767.0f;
    m_stickY = static_cast<float>(left.y) / 32767.0f;

    // The stick drives the same directional events as the d-pad so either
    // works without the screens knowing which was used.
    const bool dirNow[4] = {
        (m_held & HidNpadButton_Left) != 0 || left.x < -kStickDeadzone,
        (m_held & HidNpadButton_Right) != 0 || left.x > kStickDeadzone,
        (m_held & HidNpadButton_Up) != 0 || left.y > kStickDeadzone,
        (m_held & HidNpadButton_Down) != 0 || left.y < -kStickDeadzone,
    };

    updateTouch();

    // Whichever was used last owns the cursor. Checked before the repeat
    // pass so a held direction keeps the pad in charge.
    if (m_down != 0 || (m_held & (HidNpadButton_AnyLeft | HidNpadButton_AnyRight |
                                  HidNpadButton_AnyUp | HidNpadButton_AnyDown)) != 0 ||
        std::fabs(m_stickX) > 0.3f || std::fabs(m_stickY) > 0.3f)
        m_touchDriven = false;

    for (int i = 0; i < 4; i++) {
        m_navigate[i] = false;
        if (!dirNow[i]) {
            m_dirHeld[i] = false;
            m_dirTimer[i] = 0.0f;
            continue;
        }
        if (!m_dirHeld[i]) {
            m_dirHeld[i] = true;
            m_dirTimer[i] = kRepeatDelay;
            m_navigate[i] = true;
            continue;
        }
        m_dirTimer[i] -= dt;
        if (m_dirTimer[i] <= 0.0f) {
            m_dirTimer[i] += kRepeatRate;
            m_navigate[i] = true;
        }
    }
}

void Input::updateTouch()
{
    HidTouchScreenState state{};
    const bool any = hidGetTouchScreenStates(&state, 1) && state.count > 0;

    m_touchDown = any && !m_touchActive;
    m_touchUp = !any && m_touchActive;
    m_touchDX = m_touchDY = 0.0f;

    if (!any) {
        m_touchActive = false;
        // x/y are deliberately left where the finger lifted: tapped() is
        // answered on the frame of the release and needs that position.
        return;
    }

    // The first finger only. Multi-touch would mean gestures this UI has no
    // use for, and tracking finger ids to keep a drag on the right one is
    // complexity for nothing.
    const float x = static_cast<float>(state.touches[0].x) / m_scale;
    const float y = static_cast<float>(state.touches[0].y) / m_scale;

    if (m_touchDown) {
        m_touchDriven = true;
        m_touchStartX = x;
        m_touchStartY = y;
        m_tapCandidate = true;
    } else {
        m_touchDX = x - m_touchX;
        m_touchDY = y - m_touchY;
        const float dx = x - m_touchStartX;
        const float dy = y - m_touchStartY;
        if (dx * dx + dy * dy > kTapSlop * kTapSlop) m_tapCandidate = false;
    }
    m_touchX = x;
    m_touchY = y;
    m_touchActive = true;
}

bool Input::anyNavigate() const
{
    return m_navigate[0] || m_navigate[1] || m_navigate[2] || m_navigate[3];
}

} // namespace ui
