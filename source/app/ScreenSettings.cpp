#include <algorithm>
#include <string>
#include <vector>

#include <switch.h>

#include "app/App.h"
#include "app/Screens.h"
#include "net/Api.h"
#include "player/Player.h"

namespace app {

using theme::metric::BodyFont;
using theme::metric::RadiusField;

namespace {

constexpr uint32_t kKindRow = 400;
constexpr float kRowH = 56.0f;
constexpr float kRowGap = 10.0f;

class SettingsScreen final : public Screen {
public:
    std::string title() const override { return "Settings"; }

    bool handleInput(App& app) override
    {
        if (app.drawerOpen()) return false;
        if (!app.input().pressed(HidNpadButton_A)) return false;

        const ui::FocusId id = app.focus().current();
        const uint32_t index = static_cast<uint32_t>(id & 0xffffffffu);
        if (static_cast<uint32_t>(id >> 32) != kKindRow) return false;

        switch (index) {
            case 0: {
                // Step through the rungs this server actually publishes.
                // Before the first playback there is no ladder to step
                // through, so the row says so and does nothing.
                const std::vector<api::QualityRung> rungs = api::knownQualities();
                if (rungs.empty()) break;
                player::Settings s = player::settings();
                size_t at = rungs.size();
                for (size_t i = 0; i < rungs.size(); i++)
                    if (rungs[i].id == s.quality) at = i;
                s.quality = rungs[(at + 1) % rungs.size()].id;
                player::setSettings(s);
                break;
            }
            case 1: {
                player::Settings s = player::settings();
                s.showStats = !s.showStats;
                player::setSettings(s);
                break;
            }
            case 2:
                app.client().logout();
                app.replace(makeServerScreen());
                break;
            default:
                break;
        }
        return true;
    }

    // What the ladder calls the rung, with the bitrate that decides whether a
    // link can carry it. Falls back to the bare id until a playback-info call
    // has told us what this server offers.
    static std::string qualityRowValue(const std::string& id)
    {
        for (const api::QualityRung& rung : api::knownQualities())
            if (rung.id == id) return api::qualityDescription(rung);
        return id.empty() ? "Auto" : id;
    }

    void draw(App& app) override
    {
        ui::Context& ctx = app.ui();
        const gfx::Rect content = app.contentRect();
        const float width = std::min(560.0f, content.w);
        const player::Settings settings = player::settings();

        struct Row {
            std::string label;
            std::string value;
            bool actionable;
        };
        std::vector<Row> rows;
        rows.push_back(Row{ "Quality", qualityRowValue(settings.quality), true });
        rows.push_back(Row{ "Playback statistics", settings.showStats ? "On" : "Off", true });
        rows.push_back(Row{ "Sign out", app.client().currentUser().username, true });
        rows.push_back(Row{ "Server", app.client().server(), false });
        rows.push_back(Row{ "TLS verification",
                            net::caBundleLoaded() ? "CA bundle loaded" : "No CA bundle", false });
        rows.push_back(Row{ "Version", APP_VERSION, false });

        ctx.focus->beginContainer(0x5e771000, false);
        float y = content.y + 8.0f;
        for (size_t i = 0; i < rows.size(); i++) {
            const gfx::Rect box{ content.x, y, width, kRowH };
            if (rows[i].actionable) {
                const ui::FocusId id = ui::focusId(kKindRow, static_cast<uint32_t>(i));
                ctx.focus->add(id, box);
                if (ctx.focus->isFocused(id)) ui::focusRing(ctx, box, RadiusField);
            }
            ctx.r->fillRect(box, rows[i].actionable ? theme::Base200 : theme::Base200.withAlpha(0.5f),
                            RadiusField);

            gfx::TextStyle label;
            label.size = BodyFont;
            label.color = theme::BaseContent.withAlpha(rows[i].actionable ? 1.0f : 0.6f);
            ctx.text->draw(rows[i].label, box.x + 16,
                           box.cy() - ctx.text->lineHeight(BodyFont) * 0.5f, label);

            gfx::TextStyle value;
            value.size = 15.0f;
            value.color = theme::BaseContent.withAlpha(0.6f);
            ctx.text->drawEllipsized(rows[i].value,
                                     gfx::Rect{ box.x + width * 0.45f,
                                                box.cy() - ctx.text->lineHeight(15.0f) * 0.5f,
                                                width * 0.55f - 16.0f,
                                                ctx.text->lineHeight(15.0f) },
                                     value, ui::Align::Right);
            y += kRowH + kRowGap;
        }
        ctx.focus->endContainer();

        gfx::TextStyle hint;
        hint.size = 13.0f;
        hint.color = theme::BaseContent.withAlpha(0.4f);
        ctx.text->draw(api::knownQualities().empty()
                           ? "The quality list comes from the server and appears once "
                             "something has been played."
                           : "Decoding runs on the video hardware, so the link is what "
                             "limits this, not the console: direct play at 1080p needs "
                             "wired networking or a server on the LAN. The eco rungs are "
                             "full resolution at roughly half the bitrate.",
                       content.x, y + 12.0f, hint);
    }
};

} // namespace

std::unique_ptr<Screen> makeSettingsScreen() { return std::make_unique<SettingsScreen>(); }

} // namespace app
