#pragma once

#include <algorithm>
#include <cmath>

#include "gfx/Color.h"

namespace ui {

// The client's rails scroll with `scroll-behavior: smooth` and stop the
// focused card `scroll-px-16` (64px) short of the edge, so a neighbour always
// peeks in and the row never looks like it ends at the screen border.
class Scroller {
public:
    static constexpr float kPeek = 64.0f;

    void reset()
    {
        m_offset = 0.0f;
        m_target = 0.0f;
    }

    void setExtent(float content, float viewport)
    {
        m_max = std::max(0.0f, content - viewport);
        m_target = std::min(m_target, m_max);
        m_offset = std::min(m_offset, m_max);
    }

    // `item` and `viewport` share the un-scrolled coordinate space.
    void revealX(gfx::Rect item, gfx::Rect viewport)
    {
        const float left = item.x - viewport.x - kPeek;
        const float right = item.right() - viewport.x - viewport.w + kPeek;
        if (left < m_target) m_target = left;
        else if (right > m_target) m_target = right;
        m_target = std::max(0.0f, std::min(m_target, m_max));
    }

    void revealY(gfx::Rect item, gfx::Rect viewport, float margin = 24.0f)
    {
        const float top = item.y - viewport.y - margin;
        const float bottom = item.bottom() - viewport.y - viewport.h + margin;
        if (top < m_target) m_target = top;
        else if (bottom > m_target) m_target = bottom;
        m_target = std::max(0.0f, std::min(m_target, m_max));
    }

    // Touch drag. The finger moves content directly — no settle — because
    // anything else feels like the panel is ignoring you. `delta` is the
    // finger's movement, so the content goes the other way.
    void dragBy(float delta)
    {
        m_dragging = true;
        m_offset = std::max(0.0f, std::min(m_offset - delta, m_max));
        m_target = m_offset;
        // Averaged over a few frames: a single frame's delta is noisy enough
        // that a flick sometimes ends on a near-zero sample and goes nowhere.
        m_velocity = m_velocity * 0.7f - delta * 0.3f;
    }

    // Carries the flick on after the finger leaves, then settles.
    void endDrag()
    {
        m_dragging = false;
        if (std::fabs(m_velocity) < 1.0f) {
            m_velocity = 0.0f;
            return;
        }
        m_target = std::max(0.0f, std::min(m_offset + m_velocity * kFlingScale, m_max));
        m_velocity = 0.0f;
    }

    bool dragging() const { return m_dragging; }

    void animate(float dt)
    {
        if (m_dragging) return;
        // Exponential settle, frame-rate independent; ~120ms to close the gap.
        const float t = 1.0f - std::exp(-18.0f * dt);
        m_offset += (m_target - m_offset) * t;
        if (std::fabs(m_target - m_offset) < 0.25f) m_offset = m_target;
    }

    float offset() const { return m_offset; }
    float max() const { return m_max; }
    bool atStart() const { return m_offset <= 0.5f; }
    bool atEnd() const { return m_offset >= m_max - 0.5f; }

private:
    // Turns a per-frame velocity into a distance to coast. Tuned so a brisk
    // flick crosses roughly a screen.
    static constexpr float kFlingScale = 14.0f;

    float m_offset = 0.0f;
    float m_target = 0.0f;
    float m_max = 0.0f;
    float m_velocity = 0.0f;
    bool m_dragging = false;
};

} // namespace ui
