#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <switch.h>

#include "app/App.h"
#include "app/Screens.h"
#include "ui/Scroller.h"

namespace app {

using theme::metric::CardPortraitW;
using theme::metric::RailGap;
using theme::metric::RailTitleFont;

namespace {

struct Liveness {
    std::atomic<bool> alive{ true };
};

constexpr uint32_t kKindCredit = 500;
constexpr float kAvatar = 132.0f;

// A person's credits *in this library*, not their whole filmography — the
// server only joins what it actually holds, which is what makes the page a
// way into something to watch rather than a reference work.
class PersonScreen final : public Screen {
public:
    explicit PersonScreen(int personId)
        : m_live(std::make_shared<Liveness>()), m_personId(personId)
    {
    }
    ~PersonScreen() override { m_live->alive = false; }

    std::string title() const override { return m_person.name; }
    std::string backgroundUrl() const override { return m_backdrop; }

    void onEnter(App& app) override { load(app); }
    void update(App&, float dt) override { m_scroll.animate(dt); }

    bool handleInput(App& app) override
    {
        if (app.drawerOpen() || !m_loaded) return false;
        ui::Input& in = app.input();

        if (ui::touchScroll(in, m_scroll, app.contentRect())) return true;
        if (!in.pressed(HidNpadButton_A)) return false;

        const ui::FocusId id = app.focus().current();
        if (static_cast<uint32_t>(id >> 32) != kKindCredit) return false;
        const uint32_t index = static_cast<uint32_t>(id & 0xffffffffu);
        if (index >= m_credits.size()) return false;
        app.push(makeDetailScreen(m_credits[index].media.id));
        return true;
    }

    void draw(App& app) override
    {
        ui::Context& ctx = app.ui();
        const gfx::Rect content = app.contentRect();

        if (!m_loaded) {
            ui::spinner(ctx, gfx::Rect{ content.cx() - 18, content.cy() - 18, 36, 36 },
                        theme::Primary, ctx.time);
            return;
        }

        ctx.r->pushClip(gfx::Rect{ 0, content.y, ctx.r->width(), content.h });
        ctx.focus->beginContainer(0x9e120000, false);

        float y = content.y + 8.0f - m_scroll.offset();
        y = drawHeader(app, content, y);
        y = drawCredits(app, content, y);

        ctx.focus->endContainer();
        ctx.r->popClip();

        const float total = y - (content.y + 8.0f - m_scroll.offset());
        m_scroll.setExtent(total + 16.0f, content.h);
        followFocus(app, content);
    }

private:
    float drawHeader(App& app, gfx::Rect content, float y)
    {
        ui::Context& ctx = app.ui();
        const gfx::Rect avatar{ content.x, y, kAvatar, kAvatar };

        ctx.r->fillRect(avatar, theme::Base300, kAvatar * 0.5f);
        const int decodeWidth = static_cast<int>(kAvatar * ctx.r->scale() + 0.5f);
        const gfx::TexId tex = m_person.avatarUrl.empty()
                                   ? gfx::TexInvalid
                                   : ctx.images->get(m_person.avatarUrl, "medium", decodeWidth);
        if (tex != gfx::TexInvalid)
            ctx.r->drawImage(tex, avatar, theme::White, kAvatar * 0.5f,
                             ui::coverUv(ctx.r->textureWidth(tex), ctx.r->textureHeight(tex),
                                         avatar));
        else
            ctx.icons->draw(*ctx.r, ui::Icon::User,
                            gfx::Rect{ avatar.cx() - 28, avatar.cy() - 28, 56, 56 },
                            theme::BaseContent.withAlpha(0.3f));

        const float tx = avatar.right() + 24.0f;
        const float tw = content.right() - tx;
        float ty = y + 4.0f;

        gfx::TextStyle name;
        name.size = 30.0f;
        name.bold = true;
        name.color = theme::BaseContent;
        ctx.text->drawEllipsized(m_person.name, gfx::Rect{ tx, ty, tw, 36.0f }, name);
        ty += 40.0f;

        // Born / died / from, as one line — the client's own subtitle row.
        std::string facts = m_person.knownForDepartment;
        auto append = [&facts](const std::string& piece) {
            if (piece.empty()) return;
            if (!facts.empty()) facts += "  ·  ";
            facts += piece;
        };
        append(yearOf(m_person.birthday).empty()
                   ? std::string()
                   : (m_person.deathday.empty()
                          ? "Born " + yearOf(m_person.birthday)
                          : yearOf(m_person.birthday) + "–" + yearOf(m_person.deathday)));
        append(m_person.placeOfBirth);
        if (!facts.empty()) {
            gfx::TextStyle sub;
            sub.size = 15.0f;
            sub.color = theme::BaseContent.withAlpha(0.6f);
            ctx.text->drawEllipsized(facts, gfx::Rect{ tx, ty, tw, 22.0f }, sub);
            ty += 28.0f;
        }

        if (!m_person.biography.empty()) {
            gfx::TextStyle bio;
            bio.size = 15.0f;
            bio.color = theme::BaseContent.withAlpha(0.8f);
            // Four lines beside the portrait; the rest is not worth a scroll
            // on a browse surface.
            ty += ctx.text->drawWrapped(m_person.biography, gfx::Rect{ tx, ty, tw, 96.0f }, bio, 4);
        }

        return std::max(avatar.bottom(), ty) + 24.0f;
    }

    float drawCredits(App& app, gfx::Rect content, float y)
    {
        if (m_credits.empty()) return y;
        ui::Context& ctx = app.ui();

        ui::railTitle(ctx, m_creditsTitle, gfx::Rect{ content.x, y, content.w, 24.0f });
        y += ctx.text->lineHeight(RailTitleFont) + theme::metric::RailTitleGap;

        const float cardW = CardPortraitW;
        const float step = cardW + RailGap;
        const int columns = std::max(1, static_cast<int>((content.w + RailGap) / step));
        ui::CardView probe;
        probe.subtitle = " ";
        const float cellH = ui::cardBounds(probe, 0, 0, cardW, true, ctx).h + 18.0f;

        const int rows = (static_cast<int>(m_credits.size()) + columns - 1) / columns;
        for (int row = 0; row < rows; row++) {
            const float rowY = y + row * cellH;
            if (rowY > content.bottom() + cellH || rowY + cellH < content.y - cellH) {
                // Off screen, but the cards still register so the D-pad can
                // reach them and scrolling lands somewhere sensible.
                ctx.focus->beginContainer(0x9e130000u + static_cast<uint32_t>(row), true);
                for (int col = 0; col < columns; col++) {
                    const int index = row * columns + col;
                    if (index >= static_cast<int>(m_credits.size())) break;
                    ctx.focus->add(ui::focusId(kKindCredit, static_cast<uint32_t>(index)),
                                   gfx::Rect{ content.x + col * step, rowY, cardW, cellH - 18.0f });
                }
                ctx.focus->endContainer();
                continue;
            }

            ctx.focus->beginContainer(0x9e130000u + static_cast<uint32_t>(row), true);
            for (int col = 0; col < columns; col++) {
                const int index = row * columns + col;
                if (index >= static_cast<int>(m_credits.size())) break;
                const gfx::Rect box{ content.x + col * step, rowY, cardW, cellH - 18.0f };
                const ui::FocusId id = ui::focusId(kKindCredit, static_cast<uint32_t>(index));
                ctx.focus->add(id, box);

                const api::PersonCredit& credit = m_credits[index];
                ui::CardView card;
                card.title = credit.media.title;
                // The role, not the year: on this page it is the thing that
                // differs between one card and the next.
                card.subtitle = credit.role.empty() ? credit.media.subtitle : credit.role;
                card.imageUrl = credit.media.posterUrl;
                card.rating = credit.media.rating;
                card.watched = credit.media.watched;
                card.progress = credit.media.progressPercent;
                card.unavailable = !credit.media.available;
                ui::mediaCard(ctx, card, box, ctx.focus->isFocused(id));
            }
            ctx.focus->endContainer();
        }
        return y + rows * cellH;
    }

    void followFocus(App& app, gfx::Rect content)
    {
        if (app.input().touchDriven()) return;
        const ui::FocusId id = app.focus().current();
        if (id == ui::FocusNone) return;
        const gfx::Rect item = app.focus().rectOf(id);
        if (item.empty()) return;
        m_scroll.revealY(gfx::Rect{ item.x, item.y + m_scroll.offset(), item.w, item.h }, content,
                         24.0f);
    }

    static std::string yearOf(const std::string& iso)
    {
        return iso.size() >= 4 ? iso.substr(0, 4) : std::string();
    }

    void load(App& app)
    {
        auto live = m_live;
        auto* tasks = &app.tasks();
        App* appPtr = &app;
        const int id = m_personId;
        tasks->post(
            [this, live, tasks, appPtr, id] {
                api::PersonDetail detail;
                const bool ok = appPtr->client().fetchPerson(id, detail);
                tasks->postToMain([this, live, ok, detail, appPtr] {
                    if (!live->alive) return;
                    if (ok) m_person = detail;

                    // Cast first, then crew: an actor's roles are what anyone
                    // came here for, and a director's credits fill the same
                    // grid when there are no acting ones.
                    m_credits = m_person.cast;
                    m_credits.insert(m_credits.end(), m_person.crew.begin(), m_person.crew.end());
                    m_creditsTitle = m_person.cast.empty() ? "Crew" : "Appears in";
                    for (const api::PersonCredit& credit : m_credits)
                        if (m_backdrop.empty()) m_backdrop = credit.media.fanartUrl;

                    m_loaded = true;
                    // Nothing was focusable while the spinner was up, so the
                    // cursor has to be placed once the grid exists.
                    appPtr->focus().focusFirst();
                });
            },
            util::TaskQueue::Priority::High);
    }

    std::shared_ptr<Liveness> m_live;
    api::PersonDetail m_person;
    std::vector<api::PersonCredit> m_credits;
    std::string m_creditsTitle = "Appears in";
    std::string m_backdrop;
    ui::Scroller m_scroll;
    int m_personId = 0;
    bool m_loaded = false;
};

} // namespace

std::unique_ptr<Screen> makePersonScreen(int personId)
{
    return std::make_unique<PersonScreen>(personId);
}

} // namespace app
