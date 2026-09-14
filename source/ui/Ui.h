#pragma once

#include <string>

#include "app/Theme.h"
#include "gfx/ImageStore.h"
#include "gfx/Renderer.h"
#include "gfx/Text.h"
#include "ui/Focus.h"
#include "ui/Input.h"

namespace api {
class Client;
}
namespace util {
class TaskQueue;
}

namespace ui {

class Input;
class Scroller;

// Drag-to-scroll for a screen's list. Returns true while a drag owns the
// touch, which is the screen's signal to swallow the frame's input so a
// flick over a card does not also activate it.
bool touchScroll(Input& in, Scroller& scroll, gfx::Rect viewport, bool horizontal = false);

// Text alignment belongs to the renderer, but every call site is a widget,
// so it is spelled ui::Align alongside the rest of them.
using Align = gfx::Align;

// Atlas order must match tools/build_icons.py ICONS.
enum class Icon {
    Play, Pause, Star, Check, ChevronLeft, ChevronRight, ChevronUp, ChevronDown,
    Film, Heart, Search, Home, Library, Clapperboard, Tv, Settings, User,
    ListPlus, Plus, X, CircleX, Clock, Rocket, Cast,
    Menu, Pin, EllipsisVertical, RotateCcw, ArrowLeft, SkipForward, SkipBack, Loader,
    Count
};

class IconSet {
public:
    bool load(gfx::Renderer& renderer, const char* path);
    void draw(gfx::Renderer& renderer, Icon icon, gfx::Rect box, gfx::Color color) const;

private:
    gfx::TexId m_tex = gfx::TexInvalid;
    int m_cols = 8;
    int m_cell = 96;
};

struct Context {
    gfx::Renderer* r = nullptr;
    gfx::TextRenderer* text = nullptr;
    gfx::ImageStore* images = nullptr;
    IconSet* icons = nullptr;
    FocusManager* focus = nullptr;
    Input* input = nullptr;
    api::Client* api = nullptr;
    util::TaskQueue* tasks = nullptr;
    float dt = 1.0f / 60.0f;
    double time = 0.0;
};

// `0 0 0 2px base-100, 0 0 0 6px primary` — the client's "lit-from-within"
// card ring, with the blurred layers dropped exactly as `body.tv` does.
void focusRing(Context& ctx, gfx::Rect box, float radius);

enum class ButtonStyle { Primary, Ghost, Neutral };

// Draws the button and reports whether it is focused; activation is the
// caller's, so a screen keeps its own A-button semantics.
bool button(Context& ctx, FocusId id, gfx::Rect box, const std::string& label,
            ButtonStyle style = ButtonStyle::Ghost, Icon leading = Icon::Count);

float buttonWidth(Context& ctx, const std::string& label, bool hasIcon);

struct CardView {
    std::string title;
    std::string subtitle;
    std::string imageUrl;
    double rating = 0;
    bool watched = false;
    float progress = 0;
    bool landscape = false;
    bool unavailable = false;
    bool hideCaption = false;
};

// `app-media-card`: artwork figure, badges, progress bar and the caption
// underneath. `box` is the figure; the caption is drawn below it.
void mediaCard(Context& ctx, const CardView& card, gfx::Rect box, bool focused);

gfx::Rect cardBounds(const CardView& card, float x, float y, float width, bool withCaption,
                     Context& ctx);

void railTitle(Context& ctx, const std::string& title, gfx::Rect box);
void spinner(Context& ctx, gfx::Rect box, gfx::Color color, double time);

// object-cover: the crop the client's `object-cover` produces for a texture
// of `texW x texH` shown in `box`.
gfx::Rect coverUv(uint32_t texW, uint32_t texH, gfx::Rect box);

std::string formatRuntime(int minutes);
std::string formatTime(double seconds);

} // namespace ui
