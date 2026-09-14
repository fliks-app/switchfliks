#include "ui/Ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "gfx/ImageDecode.h"
#include "ui/Input.h"
#include "ui/Scroller.h"

namespace ui {

namespace {
// Below this the finger is still making a tap, and stealing it would make
// every card hard to press.
// Above Input's tap slop, so a drag has always cancelled the tap by the
// time it starts scrolling — otherwise a short drag both scrolls and
// activates whatever was under the finger.
constexpr float kDragThreshold = 14.0f;
} // namespace

using theme::metric::CardCaptionGap;
using theme::metric::CardSubFont;
using theme::metric::CardTitleFont;
using theme::metric::FocusGap;
using theme::metric::FocusRing;
using theme::metric::RadiusBox;

namespace {

std::string readWholeFile(const char* path)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::string out;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

// text-yellow-400, the one colour on the card that is not a daisyUI token.
constexpr gfx::Color kRatingStar = gfx::Color::rgb(0xfacc15);

} // namespace

bool IconSet::load(gfx::Renderer& renderer, const char* path)
{
    const std::string bytes = readWholeFile(path);
    if (bytes.empty()) return false;

    gfx::DecodedImage decoded;
    if (!gfx::decodeImage(bytes, 0, decoded) || !decoded.valid()) return false;

    // The atlas is authored as greyscale coverage; only the red channel is
    // kept so it uploads as R8 and the shader's alpha-mask mode reads it.
    std::vector<uint8_t> alpha(static_cast<size_t>(decoded.width) * decoded.height);
    for (size_t i = 0; i < alpha.size(); i++) alpha[i] = decoded.rgba[i * 4];

    m_cell = decoded.width / m_cols;
    m_tex = renderer.createTexture(decoded.width, decoded.height, DkImageFormat_R8_Unorm,
                                   alpha.data(), gfx::Filter::Linear);
    return m_tex != gfx::TexInvalid;
}

void IconSet::draw(gfx::Renderer& renderer, Icon icon, gfx::Rect box, gfx::Color color) const
{
    if (m_tex == gfx::TexInvalid || icon == Icon::Count) return;
    const int index = static_cast<int>(icon);
    const float texW = static_cast<float>(renderer.textureWidth(m_tex));
    const float texH = static_cast<float>(renderer.textureHeight(m_tex));
    if (texW <= 0 || texH <= 0) return;

    const float u = (index % m_cols) * m_cell / texW;
    const float v = (index / m_cols) * m_cell / texH;
    renderer.drawAlphaQuad(m_tex, box, gfx::Rect{ u, v, m_cell / texW, m_cell / texH }, color);
}

void focusRing(Context& ctx, gfx::Rect box, float radius)
{
    ctx.r->fillRect(box.expand(FocusRing), theme::Primary, radius + FocusRing);
    ctx.r->fillRect(box.expand(FocusGap), theme::Base100, radius + FocusGap);
}

gfx::Rect coverUv(uint32_t texW, uint32_t texH, gfx::Rect box)
{
    if (texW == 0 || texH == 0 || box.empty()) return gfx::Rect{ 0, 0, 1, 1 };
    const float texAspect = static_cast<float>(texW) / static_cast<float>(texH);
    const float boxAspect = box.w / box.h;
    if (texAspect > boxAspect) {
        const float w = boxAspect / texAspect;
        return gfx::Rect{ (1.0f - w) * 0.5f, 0.0f, w, 1.0f };
    }
    const float h = texAspect / boxAspect;
    return gfx::Rect{ 0.0f, (1.0f - h) * 0.5f, 1.0f, h };
}

float buttonWidth(Context& ctx, const std::string& label, bool hasIcon)
{
    const float text = ctx.text->measure(label, theme::metric::BodyFont, false);
    return text + (hasIcon ? 26.0f : 0.0f) + 32.0f;
}

bool button(Context& ctx, FocusId id, gfx::Rect box, const std::string& label, ButtonStyle style,
            Icon leading)
{
    const bool focused = ctx.focus->isFocused(id);
    ctx.focus->add(id, box);

    gfx::Color bg = theme::Base200;
    gfx::Color fg = theme::BaseContent;
    switch (style) {
        case ButtonStyle::Primary:
            bg = theme::Primary;
            fg = theme::PrimaryContent;
            break;
        case ButtonStyle::Ghost:
            // daisyUI's ghost is transparent until interacted with.
            bg = focused ? theme::BaseContent.withAlpha(0.12f) : theme::Base100.withAlpha(0.0f);
            break;
        case ButtonStyle::Neutral:
            bg = theme::Base200;
            break;
    }

    if (focused) focusRing(ctx, box, theme::metric::RadiusField);
    if (bg.a > 0.0f) ctx.r->fillRect(box, bg, theme::metric::RadiusField);

    const bool hasIcon = leading != Icon::Count;
    const float iconSize = 18.0f;
    const float textW = ctx.text->measure(label, theme::metric::BodyFont, false);
    const float contentW = textW + (hasIcon ? iconSize + 8.0f : 0.0f);
    float x = box.x + (box.w - contentW) * 0.5f;

    if (hasIcon) {
        ctx.icons->draw(*ctx.r, leading,
                        gfx::Rect{ x, box.cy() - iconSize * 0.5f, iconSize, iconSize }, fg);
        x += iconSize + 8.0f;
    }
    gfx::TextStyle ts;
    ts.size = theme::metric::BodyFont;
    ts.color = fg;
    ts.bold = style == ButtonStyle::Primary;
    ctx.text->draw(label, x, box.cy() - ctx.text->lineHeight(ts.size) * 0.5f, ts);
    return focused;
}

gfx::Rect cardBounds(const CardView& card, float x, float y, float width, bool withCaption,
                     Context& ctx)
{
    const float figureH = card.landscape ? width * 9.0f / 16.0f : width * 3.0f / 2.0f;
    float total = figureH;
    if (withCaption && !card.hideCaption) {
        total += CardCaptionGap + ctx.text->lineHeight(CardTitleFont);
        if (!card.subtitle.empty()) total += ctx.text->lineHeight(CardSubFont);
    }
    return gfx::Rect{ x, y, width, total };
}

void mediaCard(Context& ctx, const CardView& card, gfx::Rect box, bool focused)
{
    const float figureH = card.landscape ? box.w * 9.0f / 16.0f : box.w * 3.0f / 2.0f;
    const gfx::Rect figure{ box.x, box.y, box.w, figureH };

    if (focused) focusRing(ctx, figure, RadiusBox);
    ctx.r->fillRect(figure, theme::Base300, RadiusBox);

    // Decode at the drawn size in device pixels; a rail card never needs the
    // full-size artwork the server also offers.
    const int decodeWidth = static_cast<int>(box.w * ctx.r->scale() + 0.5f);
    const char* variant = card.landscape ? "thumb" : "thumb";
    const gfx::TexId tex = card.imageUrl.empty()
                               ? gfx::TexInvalid
                               : ctx.images->get(card.imageUrl, variant, decodeWidth);

    if (tex != gfx::TexInvalid) {
        const gfx::Rect uv = coverUv(ctx.r->textureWidth(tex), ctx.r->textureHeight(tex), figure);
        const float reveal =
            ctx.images->revealAlpha(gfx::ImageStore::keyFor(card.imageUrl, variant, decodeWidth));
        gfx::Color tint = theme::White.withAlpha(reveal);
        if (card.unavailable) tint = tint.dim(0.55f);
        ctx.r->drawImage(tex, figure, tint, RadiusBox, uv);
    } else {
        ctx.icons->draw(*ctx.r, Icon::Film,
                        gfx::Rect{ figure.cx() - 24, figure.cy() - 24, 48, 48 },
                        theme::BaseContent.withAlpha(0.20f));
    }

    ctx.r->pushClip(figure);

    if (card.unavailable) {
        ctx.icons->draw(*ctx.r, Icon::CircleX, gfx::Rect{ figure.x + 8, figure.y + 8, 20, 20 },
                        theme::Error);
    }

    if (card.rating > 0 && !card.landscape) {
        char label[16];
        std::snprintf(label, sizeof(label), "%.1f", card.rating);
        const float textW = ctx.text->measure(label, 11.0f, true);
        const gfx::Rect badge{ figure.x + 6, figure.bottom() - 6 - 18, textW + 12 + 14, 18 };
        ctx.r->fillRect(badge, theme::Black.withAlpha(0.6f), 6.0f);
        ctx.icons->draw(*ctx.r, Icon::Star, gfx::Rect{ badge.x + 5, badge.cy() - 6, 12, 12 },
                        kRatingStar);
        gfx::TextStyle ts;
        ts.size = 11.0f;
        ts.bold = true;
        ts.color = theme::White;
        ctx.text->draw(label, badge.x + 20, badge.cy() - ctx.text->lineHeight(11.0f) * 0.5f, ts);
    }

    if (card.watched) {
        const gfx::Rect dot{ figure.right() - 6 - 20, figure.y + 6, 20, 20 };
        ctx.r->fillRect(dot, theme::Success, 10.0f);
        ctx.icons->draw(*ctx.r, Icon::Check, dot.inset(4.0f), theme::SuccessContent);
    }

    if (card.progress > 0.0f) {
        const gfx::Rect track{ figure.x, figure.bottom() - 4, figure.w, 4 };
        ctx.r->fillRect(track, theme::Black.withAlpha(0.4f), 0.0f);
        gfx::Rect fill = track;
        fill.w = track.w * std::min(card.progress, 100.0f) / 100.0f;
        ctx.r->fillRect(fill, theme::Primary, 0.0f);
    }

    ctx.r->popClip();

    if (card.hideCaption) return;

    float y = figure.bottom() + CardCaptionGap;
    gfx::TextStyle title;
    title.size = CardTitleFont;
    title.color = theme::BaseContent;
    ctx.text->drawEllipsized(card.title,
                             gfx::Rect{ box.x, y, box.w, ctx.text->lineHeight(CardTitleFont) },
                             title);

    if (!card.subtitle.empty()) {
        y += ctx.text->lineHeight(CardTitleFont);
        gfx::TextStyle sub;
        sub.size = CardSubFont;
        sub.color = theme::BaseContent.withAlpha(0.5f);
        ctx.text->drawEllipsized(card.subtitle,
                                 gfx::Rect{ box.x, y, box.w, ctx.text->lineHeight(CardSubFont) },
                                 sub);
    }
}

void railTitle(Context& ctx, const std::string& title, gfx::Rect box)
{
    gfx::TextStyle ts;
    ts.size = theme::metric::RailTitleFont;
    ts.bold = true;
    ts.color = theme::BaseContent;
    ctx.text->draw(title, box.x, box.y, ts);
}

void spinner(Context& ctx, gfx::Rect box, gfx::Color color, double time)
{
    // The atlas glyph cannot rotate, so the arc is traced as a ring of dots
    // whose opacity chases the angle — visually the same cue as daisyUI's
    // `.loading-spinner` at this size.
    const float radius = std::min(box.w, box.h) * 0.5f - 2.0f;
    if (radius <= 0) return;
    constexpr int kDots = 8;
    const float dotSize = radius * 0.32f;
    for (int i = 0; i < kDots; i++) {
        const float angle = static_cast<float>(i) / kDots * 6.2831853f;
        const float phase = std::fmod(static_cast<float>(time) * 1.4f + static_cast<float>(i) / kDots,
                                      1.0f);
        const gfx::Rect dot{ box.cx() + std::cos(angle) * radius - dotSize * 0.5f,
                             box.cy() + std::sin(angle) * radius - dotSize * 0.5f, dotSize,
                             dotSize };
        ctx.r->fillRect(dot, color.withAlpha(color.a * (0.15f + 0.85f * phase)), dotSize * 0.5f);
    }
}

std::string formatRuntime(int minutes)
{
    if (minutes <= 0) return {};
    char buf[32];
    if (minutes < 60) std::snprintf(buf, sizeof(buf), "%dmin", minutes);
    else std::snprintf(buf, sizeof(buf), "%dh %02d", minutes / 60, minutes % 60);
    return buf;
}

std::string formatTime(double seconds)
{
    if (seconds < 0) seconds = 0;
    const int total = static_cast<int>(seconds);
    char buf[32];
    if (total >= 3600)
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    else
        std::snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
    return buf;
}

bool touchScroll(Input& in, Scroller& scroll, gfx::Rect viewport, bool horizontal)
{
    if (scroll.max() <= 0.0f) return false;

    if (scroll.dragging()) {
        if (in.touchUp() || !in.touchActive()) {
            scroll.endDrag();
            // The release still belongs to the drag, so the caller swallows
            // it and a flick never lands as a tap on whatever is underneath.
            return true;
        }
        scroll.dragBy(horizontal ? in.touchDX() : in.touchDY());
        return true;
    }

    if (!in.touchActive() || in.touchDown()) return false;
    if (!viewport.contains(in.touchStartX(), in.touchStartY())) return false;

    // A drag only begins once the finger has travelled far enough along the
    // scrolling axis to mean it — below that it is still a tap, and stealing
    // it would make every card hard to press.
    const float moved = horizontal ? in.touchX() - in.touchStartX() : in.touchY() - in.touchStartY();
    const float other = horizontal ? in.touchY() - in.touchStartY() : in.touchX() - in.touchStartX();
    if (std::fabs(moved) < kDragThreshold || std::fabs(moved) < std::fabs(other)) return false;

    scroll.dragBy(moved);
    return true;
}

} // namespace ui
