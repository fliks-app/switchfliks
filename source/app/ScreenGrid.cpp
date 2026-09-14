#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

#include <switch.h>

#include "app/App.h"
#include "app/Screens.h"
#include "ui/Scroller.h"
#include "util/TextInput.h"

namespace app {

using theme::metric::CardPortraitW;
using theme::metric::RailGap;

namespace {

struct Liveness {
    std::atomic<bool> alive{ true };
};

// Library and search show the same poster grid; only what fills it differs.
class GridScreen : public Screen {
public:
    GridScreen() : m_live(std::make_shared<Liveness>()) {}
    ~GridScreen() override { m_live->alive = false; }

    void update(App&, float dt) override { m_scroll.animate(dt); }

    bool handleInput(App& app) override
    {
        if (app.drawerOpen()) return false;
        ui::Input& in = app.input();

        if (ui::touchScroll(in, m_scroll, app.contentRect())) return true;

        if (in.pressed(HidNpadButton_A)) {
            const ui::FocusId id = app.focus().current();
            const uint32_t index = static_cast<uint32_t>(id & 0xffffffffu);
            if (static_cast<uint32_t>(id >> 32) == kKindCard && index < m_cards.size()) {
                app.push(makeDetailScreen(m_cards[index].id));
                return true;
            }
            return onActivateOther(app, id);
        }
        return false;
    }

    void draw(App& app) override
    {
        ui::Context& ctx = app.ui();
        gfx::Rect content = app.contentRect();
        content.y += drawHeader(app, content);
        content.h = app.renderer().height() - content.y;

        if (m_loading && m_cards.empty()) {
            ui::spinner(ctx, gfx::Rect{ content.cx() - 18, content.cy() - 18, 36, 36 },
                        theme::Primary, ctx.time);
            return;
        }
        if (m_cards.empty()) {
            gfx::TextStyle ts;
            ts.size = 18.0f;
            ts.color = theme::BaseContent.withAlpha(0.5f);
            ctx.text->drawAligned(emptyMessage(), gfx::Rect{ content.x, content.cy(), content.w, 24 },
                                  ts, ui::Align::Center);
            return;
        }

        const float cardW = CardPortraitW;
        const int columns = std::max(1, static_cast<int>((content.w + RailGap) / (cardW + RailGap)));
        ui::CardView probe;
        probe.subtitle = " ";
        const float cellH = ui::cardBounds(probe, 0, 0, cardW, true, ctx).h + 20.0f;
        // Distribute the slack so the grid stays flush with the page padding
        // on both sides rather than leaving a ragged right edge.
        const float step = columns > 1 ? (content.w - cardW) / (columns - 1) : 0.0f;

        ctx.r->pushClip(content);
        ctx.focus->beginContainer(0x6a1d0001, false);

        const int rows = (static_cast<int>(m_cards.size()) + columns - 1) / columns;
        for (int row = 0; row < rows; row++) {
            const float y = content.y + row * cellH - m_scroll.offset();
            if (y + cellH < content.y - 40 || y > content.bottom() + 40) {
                // Off-screen rows still register so the D-pad can walk into
                // them; only the drawing is skipped.
                for (int col = 0; col < columns; col++) {
                    const int index = row * columns + col;
                    if (index >= static_cast<int>(m_cards.size())) break;
                    ctx.focus->add(ui::focusId(kKindCard, static_cast<uint32_t>(index)),
                                   gfx::Rect{ content.x + col * step, y, cardW, cellH - 20.0f });
                }
                continue;
            }

            ctx.focus->beginContainer(0x6a1d1000u + static_cast<uint32_t>(row), true);
            for (int col = 0; col < columns; col++) {
                const int index = row * columns + col;
                if (index >= static_cast<int>(m_cards.size())) break;
                const gfx::Rect box{ content.x + col * step, y, cardW, cellH - 20.0f };
                const ui::FocusId id = ui::focusId(kKindCard, static_cast<uint32_t>(index));
                ctx.focus->add(id, box);

                const api::MediaCard& src = m_cards[index];
                ui::CardView card;
                card.title = src.title;
                card.subtitle = src.subtitle;
                card.imageUrl = src.posterUrl;
                card.rating = src.rating;
                card.watched = src.watched;
                card.progress = src.progressPercent;
                card.unavailable = !src.available;
                ui::mediaCard(ctx, card, box, ctx.focus->isFocused(id));
            }
            ctx.focus->endContainer();
        }

        ctx.focus->endContainer();
        ctx.r->popClip();

        m_scroll.setExtent(rows * cellH, content.h);

        const ui::FocusId focused = app.focus().current();
        if (static_cast<uint32_t>(focused >> 32) == kKindCard) {
            const uint32_t index = static_cast<uint32_t>(focused & 0xffffffffu);
            const int row = static_cast<int>(index) / columns;
            // Skipped under touch: the finger owns the scroll, and pulling
            // it back to the focused cell would undo every drag.
            if (!app.input().touchDriven())
                m_scroll.revealY(
                    gfx::Rect{ content.x, content.y + row * cellH, content.w, cellH }, content,
                    16.0f);
            // One screen from the end is where the next page is worth having.
            if (index + columns * 2 >= m_cards.size()) loadMore(app);
        }
    }

protected:
    static constexpr uint32_t kKindCard = 200;
    static constexpr uint32_t kKindHeader = 201;

    virtual float drawHeader(App&, gfx::Rect) { return 0.0f; }
    virtual bool onActivateOther(App&, ui::FocusId) { return false; }
    virtual const char* emptyMessage() const { return "Nothing here"; }
    virtual api::SearchParams params() const = 0;
    virtual bool canQuery() const { return true; }

    void reload(App& app)
    {
        m_cards.clear();
        m_page = 1;
        m_total = -1;
        m_scroll.reset();
        app.focus().setCurrent(ui::FocusNone);
        loadMore(app, true);
    }

    void loadMore(App& app, bool force = false)
    {
        if (m_loading || !canQuery()) return;
        if (!force && m_total >= 0 && static_cast<int>(m_cards.size()) >= m_total) return;

        m_loading = true;
        api::SearchParams p = params();
        p.page = m_page;

        auto live = m_live;
        auto* tasks = &app.tasks();
        App* appPtr = &app;
        tasks->post(
            [this, live, tasks, appPtr, p] {
                auto page = std::make_shared<std::vector<api::MediaCard>>();
                int total = 0;
                const bool ok = appPtr->client().fetchMediaPage(p, *page, total);
                tasks->postToMain([this, live, page, total, ok] {
                    if (!live->alive) return;
                    m_loading = false;
                    if (!ok) return;
                    m_cards.insert(m_cards.end(), page->begin(), page->end());
                    m_total = total;
                    m_page++;
                });
            },
            util::TaskQueue::Priority::High);
    }

    std::shared_ptr<Liveness> m_live;
    std::vector<api::MediaCard> m_cards;
    ui::Scroller m_scroll;
    int m_page = 1;
    int m_total = -1;
    bool m_loading = false;
};

// ---------------------------------------------------------------------------

class LibraryScreen final : public GridScreen {
public:
    LibraryScreen(int libraryId, std::string name)
        : m_libraryId(libraryId), m_name(std::move(name))
    {
    }

    void onEnter(App& app) override { reload(app); }
    std::string title() const override { return m_name; }

protected:
    float drawHeader(App& app, gfx::Rect content) override
    {
        if (m_total <= 0) return 0.0f;
        gfx::TextStyle ts;
        ts.size = 14.0f;
        ts.color = theme::BaseContent.withAlpha(0.5f);
        app.text().draw(std::to_string(m_total) + " titles", content.x, content.y, ts);
        return 30.0f;
    }

    const char* emptyMessage() const override { return "This library is empty"; }

    api::SearchParams params() const override
    {
        api::SearchParams p;
        p.libraryId = m_libraryId;
        p.limit = 60;
        p.sortBy = "title";
        return p;
    }

private:
    int m_libraryId;
    std::string m_name;
};

// ---------------------------------------------------------------------------

class SearchScreen final : public GridScreen {
public:
    void onEnter(App& app) override { promptQuery(app); }
    std::string title() const override { return "Search"; }

protected:
    float drawHeader(App& app, gfx::Rect content) override
    {
        ui::Context& ctx = app.ui();
        const gfx::Rect box{ content.x, content.y, std::min(520.0f, content.w), 48.0f };
        const ui::FocusId id = ui::focusId(kKindHeader, 0);
        const bool focused = ctx.focus->isFocused(id);
        ctx.focus->add(id, box);

        if (focused) ui::focusRing(ctx, box, theme::metric::RadiusField);
        ctx.r->fillRect(box, theme::Base200, theme::metric::RadiusField);
        ctx.icons->draw(*ctx.r, ui::Icon::Search, gfx::Rect{ box.x + 14, box.cy() - 10, 20, 20 },
                        theme::BaseContent.withAlpha(0.6f));

        gfx::TextStyle ts;
        ts.size = theme::metric::BodyFont;
        ts.color = m_query.empty() ? theme::BaseContent.withAlpha(0.35f) : theme::BaseContent;
        ctx.text->drawEllipsized(m_query.empty() ? "Search your library" : m_query,
                                 gfx::Rect{ box.x + 44, box.cy() - ctx.text->lineHeight(ts.size) * 0.5f,
                                            box.w - 58, ctx.text->lineHeight(ts.size) },
                                 ts);
        return 68.0f;
    }

    bool onActivateOther(App& app, ui::FocusId id) override
    {
        if (static_cast<uint32_t>(id >> 32) != kKindHeader) return false;
        promptQuery(app);
        return true;
    }

    const char* emptyMessage() const override
    {
        return m_query.empty() ? "Press A to search" : "No results";
    }

    bool canQuery() const override { return !m_query.empty(); }

    api::SearchParams params() const override
    {
        api::SearchParams p;
        p.q = m_query;
        p.limit = 60;
        p.sortBy = "title";
        return p;
    }

private:
    void promptQuery(App& app)
    {
        std::string out;
        if (!util::textInput("Search", m_query, out)) return;
        m_query = out;
        reload(app);
    }

    std::string m_query;
};

} // namespace

std::unique_ptr<Screen> makeLibraryScreen(int libraryId, std::string name)
{
    return std::make_unique<LibraryScreen>(libraryId, std::move(name));
}

std::unique_ptr<Screen> makeSearchScreen() { return std::make_unique<SearchScreen>(); }

} // namespace app
