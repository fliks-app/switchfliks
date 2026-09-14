#pragma once
#include <cstdint>

namespace gfx {

struct Color {
    float r = 0, g = 0, b = 0, a = 1;

    static constexpr Color rgb(uint32_t hex)
    {
        return Color{ ((hex >> 16) & 0xff) / 255.0f,
                      ((hex >> 8) & 0xff) / 255.0f,
                      (hex & 0xff) / 255.0f, 1.0f };
    }

    static constexpr Color rgba(uint32_t hex, float alpha)
    {
        Color c = rgb(hex);
        c.a = alpha;
        return c;
    }

    constexpr Color withAlpha(float alpha) const { return Color{ r, g, b, alpha }; }

    // The client dims text with `text-base-content/50` etc., which is an alpha
    // on the same colour rather than a separate token.
    constexpr Color dim(float factor) const { return Color{ r, g, b, a * factor }; }

    constexpr Color mix(Color o, float t) const
    {
        return Color{ r + (o.r - r) * t, g + (o.g - g) * t,
                      b + (o.b - b) * t, a + (o.a - a) * t };
    }

    uint32_t packed() const
    {
        auto q = [](float v) -> uint32_t {
            int i = static_cast<int>(v * 255.0f + 0.5f);
            return static_cast<uint32_t>(i < 0 ? 0 : (i > 255 ? 255 : i));
        };
        return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
    }
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;

    constexpr float right() const { return x + w; }
    constexpr float bottom() const { return y + h; }
    constexpr float cx() const { return x + w * 0.5f; }
    constexpr float cy() const { return y + h * 0.5f; }
    constexpr bool empty() const { return w <= 0 || h <= 0; }

    constexpr Rect inset(float d) const { return Rect{ x + d, y + d, w - 2 * d, h - 2 * d }; }
    constexpr Rect expand(float d) const { return inset(-d); }

    constexpr bool contains(float px, float py) const
    {
        return px >= x && px < right() && py >= y && py < bottom();
    }

    Rect intersect(Rect o) const
    {
        float nx = x > o.x ? x : o.x;
        float ny = y > o.y ? y : o.y;
        float nr = right() < o.right() ? right() : o.right();
        float nb = bottom() < o.bottom() ? bottom() : o.bottom();
        return Rect{ nx, ny, nr > nx ? nr - nx : 0, nb > ny ? nb - ny : 0 };
    }
};

} // namespace gfx
