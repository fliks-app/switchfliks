#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

#include <switch.h>

#include "app/App.h"
#include "app/Screens.h"
#include "ui/Scroller.h"

namespace app {

using theme::metric::CardLandscapeW;
using theme::metric::CardPortraitW;
using theme::metric::LibraryPillH;
using theme::metric::LibraryPillW;
using theme::metric::PagePad;
using theme::metric::RadiusBox;
using theme::metric::RailGap;
using theme::metric::RailSpacing;
using theme::metric::RailTitleFont;
using theme::metric::RailTitleGap;

namespace {

struct Liveness {
    std::atomic<bool> alive{ true };
};

enum class RailKind { Libraries, Continue, Cards };

struct Rail {
    RailKind kind = RailKind::Cards;
    std::string title;
    std::vector<api::MediaCard> cards;
    bool landscape = false;
    ui::Scroller scroll;
    float y = 0;      // laid out each frame
    float height = 0;
};

// `.library-card`: a pill tinted with the library's own colour, which the
// client overlays at low opacity rather than mixing into the surface.
gfx::Color libraryTint(const std::string& hex)
{
    if (hex.size() == 7 && hex[0] == '#')
        return gfx::Color::rgb(static_cast<uint32_t>(std::strtoul(hex.c_str() + 1, nullptr, 16)));
    return theme::Primary;
}

class HomeScreen final : public Screen {
public:
    HomeScreen() : m_live(std::make_shared<Liveness>()) {}
    ~HomeScreen() override { m_live->alive = false; }

    void onEnter(App& app) override { load(app); }
    void onResume(App& app) override
    {
        // Coming back from a detail page, the resume point may have moved.
        if (m_loaded) refreshContinue(app);
    }

    std::string title() const override { return {}; }

    void update(App& app, float dt) override
    {
        m_pageScroll.animate(dt);
        for (Rail& rail : m_rails) rail.scroll.animate(dt);
        (void)app;
    }

    bool handleInput(App& app) override
    {
        if (app.drawerOpen()) return false;
        ui::Input& in = app.input();

        // A rail under the finger claims a sideways drag; anything more
        // vertical than horizontal falls through to the page. touchScroll
        // rejects the wrong axis itself, so trying both in order is enough.
        const gfx::Rect content = app.contentRect();
        for (Rail& rail : m_rails) {
            const gfx::Rect row{ 0, rail.y, app.ui().r->width(), rail.height };
            if (ui::touchScroll(in, rail.scroll, row, true)) return true;
        }
        if (ui::touchScroll(in, m_pageScroll, content)) return true;

        if (in.pressed(HidNpadButton_A)) {
            activate(app, app.focus().current());
            return true;
        }
        if (in.pressed(HidNpadButton_Y)) {
            app.push(makeSearchScreen());
            return true;
        }
        if (in.pressed(HidNpadButton_X)) {
            load(app);
            return true;
        }
        return false;
    }

    void draw(App& app) override
    {
        ui::Context& ctx = app.ui();
        const gfx::Rect content = app.contentRect();

        if (!m_loaded) {
            drawSkeleton(app, content);
            return;
        }
        if (m_rails.empty()) {
            gfx::TextStyle ts;
            ts.size = 18.0f;
            ts.color = theme::BaseContent.withAlpha(0.5f);
            ctx.text->drawAligned("Nothing here yet — add media on the server",
                                  gfx::Rect{ content.x, content.cy(), content.w, 24 }, ts,
                                  ui::Align::Center);
            return;
        }

        // Full width, not the content rect: a rail runs to the screen edges
        // and only its title keeps the page padding, which is how a 10-foot
        // browse surface is laid out everywhere. Clipping the rows to the
        // padded box left two empty strips down the sides instead.
        ctx.r->pushClip(gfx::Rect{ 0, content.y, ctx.r->width(), content.h });
        ctx.focus->beginContainer(0x480e0000, false);

        float y = content.y + 8.0f - m_pageScroll.offset();
        for (size_t i = 0; i < m_rails.size(); i++) {
            Rail& rail = m_rails[i];
            rail.y = y;
            rail.height = drawRail(app, rail, static_cast<uint32_t>(i), content, y);
            y += rail.height + RailSpacing;
        }

        ctx.focus->endContainer();
        ctx.r->popClip();

        const float contentHeight = y - (content.y + 8.0f - m_pageScroll.offset());
        m_pageScroll.setExtent(contentHeight + 16.0f, content.h);
        followFocus(app, content);
    }

private:
    static constexpr uint32_t kRailKindBase = 100;
    static ui::FocusId cardId(uint32_t rail, uint32_t index)
    {
        return ui::focusId(kRailKindBase + rail, index);
    }

    float drawRail(App& app, Rail& rail, uint32_t railIndex, gfx::Rect content, float y)
    {
        ui::Context& ctx = app.ui();
        const float titleH = ctx.text->lineHeight(RailTitleFont);

        // Rails above and below the viewport still lay out (their cards have
        // to stay reachable by the D-pad) but nothing is drawn for them.
        const float cardW = rail.kind == RailKind::Libraries
                                ? LibraryPillW
                                : (rail.landscape ? CardLandscapeW : CardPortraitW);
        ui::CardView probe;
        probe.landscape = rail.landscape;
        probe.subtitle = " ";
        const float cardH = rail.kind == RailKind::Libraries
                                ? LibraryPillH
                                : ui::cardBounds(probe, 0, 0, cardW, true, ctx).h;
        const float railH = titleH + RailTitleGap + cardH;

        const bool visible = y + railH > content.y - 40.0f && y < content.bottom() + 40.0f;
        if (visible) ui::railTitle(ctx, rail.title, gfx::Rect{ content.x, y, content.w, titleH });

        const float rowY = y + titleH + RailTitleGap;
        // Cards begin under the title but run off the right-hand edge of the
        // screen, so the usable width is everything from the padding across.
        const float screenW = ctx.r->width();
        const float railW = screenW - content.x;
        const gfx::Rect viewport{ content.x, rowY, railW, cardH };

        const int count = rail.kind == RailKind::Libraries
                              ? static_cast<int>(app.libraries().size())
                              : static_cast<int>(rail.cards.size());
        rail.scroll.setExtent(count * (cardW + RailGap) - RailGap, railW);

        ctx.focus->beginContainer(0x48100000u + railIndex, true);
        // Full width so a card scrolling past the edge is cut by the screen,
        // not by the page padding. The vertical breathing room is for the
        // focus ring, which bleeds 6px past the card.
        ctx.r->pushClip(gfx::Rect{ 0, rowY - 10, screenW, cardH + 20 });

        for (int i = 0; i < count; i++) {
            const float x = viewport.x + i * (cardW + RailGap) - rail.scroll.offset();
            const gfx::Rect box{ x, rowY, cardW, cardH };
            const ui::FocusId id = cardId(railIndex, static_cast<uint32_t>(i));
            // Registered with the un-scrolled rect so spatial nav sees the
            // whole rail, not just what is on screen.
            ctx.focus->add(id, gfx::Rect{ viewport.x + i * (cardW + RailGap), rowY, cardW, cardH });

            if (!visible || box.right() < -40 || box.x > screenW + 40) continue;
            const bool focused = ctx.focus->isFocused(id);

            if (rail.kind == RailKind::Libraries) {
                drawLibraryPill(app, app.libraries()[i], box, focused);
            } else {
                const api::MediaCard& src = rail.cards[i];
                ui::CardView card;
                card.title = src.title;
                card.subtitle = src.subtitle;
                card.imageUrl = src.posterUrl;
                card.rating = src.rating;
                card.watched = src.watched;
                card.progress = src.progressPercent;
                card.landscape = rail.landscape;
                card.unavailable = !src.available;
                ui::mediaCard(ctx, card, box, focused);
            }
        }

        ctx.r->popClip();
        ctx.focus->endContainer();
        return railH;
    }

    void drawLibraryPill(App& app, const api::Library& lib, gfx::Rect box, bool focused)
    {
        ui::Context& ctx = app.ui();
        const gfx::Color tint = libraryTint(lib.color);
        if (focused) ui::focusRing(ctx, box, 16.0f);
        ctx.r->fillRect(box, theme::Base200, 16.0f);
        ctx.r->fillRect(box, tint.withAlpha(0.12f), 16.0f);
        ctx.r->strokeRect(box, tint.withAlpha(0.35f), 16.0f, 1.0f);

        ctx.icons->draw(*ctx.r, lib.hasSeries ? ui::Icon::Tv : ui::Icon::Clapperboard,
                        gfx::Rect{ box.x + 20, box.cy() - 16, 32, 32 }, tint);
        gfx::TextStyle ts;
        ts.size = 16.0f;
        ts.bold = true;
        ts.color = theme::BaseContent;
        ctx.text->drawEllipsized(lib.name,
                                 gfx::Rect{ box.x + 60, box.cy() - ctx.text->lineHeight(16.0f) * 0.5f,
                                            box.w - 74, ctx.text->lineHeight(16.0f) },
                                 ts);
    }

    void drawSkeleton(App& app, gfx::Rect content)
    {
        ui::Context& ctx = app.ui();
        // The client's home shows a pulsing skeleton of the same shape as the
        // rails it is about to replace, so nothing jumps when data lands.
        const float pulse = 0.25f + 0.15f * static_cast<float>(std::sin(ctx.time * 3.0));
        float y = content.y + 8.0f;
        for (int row = 0; row < 3 && y < content.bottom(); row++) {
            ctx.r->fillRect(gfx::Rect{ content.x, y, 180, 20 },
                            theme::Base300.withAlpha(pulse), 4.0f);
            const float cardW = row == 1 ? CardLandscapeW : CardPortraitW;
            const float cardH = row == 1 ? CardLandscapeW * 9 / 16 : CardPortraitW * 1.5f;
            float x = content.x;
            while (x < content.right()) {
                ctx.r->fillRect(gfx::Rect{ x, y + 32, cardW, cardH },
                                theme::Base300.withAlpha(pulse), RadiusBox);
                ctx.r->fillRect(gfx::Rect{ x, y + 32 + cardH + 8, cardW * 0.75f, 14 },
                                theme::Base300.withAlpha(pulse), 4.0f);
                x += cardW + RailGap;
            }
            y += 32 + cardH + 30 + RailSpacing;
        }
    }

    void followFocus(App& app, gfx::Rect content)
    {
        // The finger owns the scroll while it is the thing driving. Pulling
        // the page back to the focus cursor after every drag was what sent
        // the home screen to the top the moment you let go.
        if (app.input().touchDriven()) return;
        const ui::FocusId id = app.focus().current();
        if (id == ui::FocusNone) return;

        for (size_t i = 0; i < m_rails.size(); i++) {
            Rail& rail = m_rails[i];
            const uint32_t kind = static_cast<uint32_t>(id >> 32);
            if (kind != kRailKindBase + i) continue;

            const gfx::Rect item = app.focus().rectOf(id);
            const float cardW = rail.kind == RailKind::Libraries
                                    ? LibraryPillW
                                    : (rail.landscape ? CardLandscapeW : CardPortraitW);
            (void)cardW;
            rail.scroll.revealX(
                item, gfx::Rect{ content.x, item.y, app.ui().r->width() - content.x, item.h });
            // The rail's own band, title included, is what scrolls into view.
            m_pageScroll.revealY(gfx::Rect{ content.x, rail.y, content.w, rail.height }, content,
                                 16.0f);
            break;
        }
    }

    void activate(App& app, ui::FocusId id)
    {
        const uint32_t kind = static_cast<uint32_t>(id >> 32);
        const uint32_t index = static_cast<uint32_t>(id & 0xffffffffu);
        if (kind < kRailKindBase || kind >= kRailKindBase + m_rails.size()) return;

        Rail& rail = m_rails[kind - kRailKindBase];
        if (rail.kind == RailKind::Libraries) {
            if (index < app.libraries().size()) {
                const api::Library& lib = app.libraries()[index];
                app.push(makeLibraryScreen(lib.id, lib.name));
            }
            return;
        }
        if (index >= rail.cards.size()) return;
        const api::MediaCard& card = rail.cards[index];

        if (rail.kind == RailKind::Continue && card.mediaFileId > 0) {
            // `clickIntent: 'play'` — the continue rail resumes rather than
            // opening the detail page.
            PlayRequest req;
            req.mediaId = card.id;
            req.mediaFileId = card.mediaFileId;
            req.episodeId = card.episodeId;
            req.startAtSeconds = card.positionSeconds;
            req.title = card.title;
            req.subtitle = card.subtitle;
            app.push(makePlayerScreen(req));
            return;
        }
        app.push(makeDetailScreen(card.id));
    }

    void load(App& app)
    {
        auto live = m_live;
        auto* tasks = &app.tasks();
        App* appPtr = &app;

        tasks->post(
            [this, live, tasks, appPtr] {
                auto libraries = std::make_shared<std::vector<api::Library>>();
                auto rails = std::make_shared<std::vector<Rail>>();

                appPtr->client().fetchLibraries(*libraries);

                // Zone order follows the client's BUILTIN_ORDER: libraries,
                // continue watching, recommendations, then recently added.
                if (!libraries->empty()) {
                    Rail r;
                    r.kind = RailKind::Libraries;
                    r.title = "Libraries";
                    rails->push_back(std::move(r));
                }
                {
                    Rail r;
                    r.kind = RailKind::Continue;
                    r.title = "Continue watching";
                    r.landscape = true;
                    if (appPtr->client().fetchContinueWatching(r.cards) && !r.cards.empty())
                        rails->push_back(std::move(r));
                }
                {
                    Rail r;
                    r.title = "Recommended for you";
                    if (appPtr->client().fetchRecommendations(r.cards) && !r.cards.empty())
                        rails->push_back(std::move(r));
                }
                {
                    Rail r;
                    r.title = "Recently added";
                    if (appPtr->client().fetchRecentlyAdded(0, 30, r.cards) && !r.cards.empty())
                        rails->push_back(std::move(r));
                }
                for (const api::Library& lib : *libraries) {
                    Rail r;
                    r.title = lib.name;
                    if (appPtr->client().fetchRecentlyAdded(lib.id, 30, r.cards) && !r.cards.empty())
                        rails->push_back(std::move(r));
                }

                tasks->postToMain([this, live, appPtr, libraries, rails] {
                    if (!live->alive) return;
                    appPtr->setLibraries(*libraries);
                    m_rails = std::move(*rails);
                    m_loaded = true;
                });
            },
            util::TaskQueue::Priority::High);
    }

    void refreshContinue(App& app)
    {
        auto live = m_live;
        auto* tasks = &app.tasks();
        App* appPtr = &app;
        tasks->post(
            [this, live, tasks, appPtr] {
                auto cards = std::make_shared<std::vector<api::MediaCard>>();
                if (!appPtr->client().fetchContinueWatching(*cards)) return;
                tasks->postToMain([this, live, cards] {
                    if (!live->alive) return;
                    for (Rail& rail : m_rails) {
                        if (rail.kind != RailKind::Continue) continue;
                        rail.cards = *cards;
                        break;
                    }
                });
            },
            util::TaskQueue::Priority::High);
    }

    std::shared_ptr<Liveness> m_live;
    std::vector<Rail> m_rails;
    ui::Scroller m_pageScroll;
    bool m_loaded = false;
};

} // namespace

std::unique_ptr<Screen> makeHomeScreen() { return std::make_unique<HomeScreen>(); }

} // namespace app
