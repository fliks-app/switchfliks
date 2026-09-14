#include <algorithm>
#include <atomic>
#include <memory>

#include <switch.h>

#include "app/App.h"
#include "app/Screens.h"
#include "util/Json.h"
#include "util/TextInput.h"

namespace app {

using theme::metric::BodyFont;
using theme::metric::RadiusBox;
using theme::metric::RadiusField;

namespace {

constexpr float kCardWidth = 440.0f;
constexpr float kRowHeight = 52.0f;
constexpr float kRowGap = 12.0f;

// The screen may be gone before a worker finishes; the flag is what lets the
// result be dropped instead of touching a destroyed object.
struct Liveness {
    std::atomic<bool> alive{ true };
};

gfx::Rect centeredCard(App& app, float height)
{
    const float x = (app.renderer().width() - kCardWidth) * 0.5f;
    const float y = std::max(theme::metric::TopBarH + 8.0f,
                             (app.renderer().height() - height) * 0.5f);
    return gfx::Rect{ x, y, kCardWidth, height };
}

// A form row: label on the left, value on the right, focus ring around the
// whole thing — the shape daisyUI's `.input` has once the TV radii apply.
void field(ui::Context& ctx, ui::FocusId id, gfx::Rect box, const std::string& label,
           const std::string& value, bool masked)
{
    const bool focused = ctx.focus->isFocused(id);
    ctx.focus->add(id, box);
    if (focused) ui::focusRing(ctx, box, RadiusField);
    ctx.r->fillRect(box, theme::Base200, RadiusField);

    gfx::TextStyle labelStyle;
    labelStyle.size = 13.0f;
    labelStyle.color = theme::BaseContent.withAlpha(0.5f);
    ctx.text->draw(label, box.x + 16, box.y + 8, labelStyle);

    std::string shown = value;
    if (masked) shown.assign(value.size(), '*');
    if (shown.empty()) shown = "—";

    gfx::TextStyle valueStyle;
    valueStyle.size = BodyFont;
    valueStyle.color = value.empty() ? theme::BaseContent.withAlpha(0.35f) : theme::BaseContent;
    ctx.text->drawEllipsized(shown,
                             gfx::Rect{ box.x + 16, box.y + 24, box.w - 32,
                                        ctx.text->lineHeight(BodyFont) },
                             valueStyle);
}

void toggleRow(ui::Context& ctx, ui::FocusId id, gfx::Rect box, const std::string& label, bool on)
{
    const bool focused = ctx.focus->isFocused(id);
    ctx.focus->add(id, box);
    if (focused) ui::focusRing(ctx, box, RadiusField);
    ctx.r->fillRect(box, theme::Base200, RadiusField);

    gfx::TextStyle ts;
    ts.size = BodyFont;
    ts.color = theme::BaseContent;
    ctx.text->drawEllipsized(label,
                             gfx::Rect{ box.x + 16, box.cy() - ctx.text->lineHeight(BodyFont) * 0.5f,
                                        box.w - 90, ctx.text->lineHeight(BodyFont) },
                             ts);

    // daisyUI `.toggle`: a pill track with a knob that slides to the end.
    const gfx::Rect track{ box.right() - 16 - 48, box.cy() - 13, 48, 26 };
    ctx.r->fillRect(track, on ? theme::Primary : theme::BaseContent.withAlpha(0.2f), 13.0f);
    const gfx::Rect knob{ on ? track.right() - 23 : track.x + 3, track.y + 3, 20, 20 };
    ctx.r->fillRect(knob, on ? theme::PrimaryContent : theme::BaseContent.withAlpha(0.75f), 10.0f);
}

// ---------------------------------------------------------------------------

class ServerScreen final : public Screen {
public:
    ServerScreen() : m_live(std::make_shared<Liveness>()) {}
    ~ServerScreen() override { m_live->alive = false; }

    void onEnter(App& app) override
    {
        m_url = app.client().server();
        if (m_url.empty()) m_url = "http://";
        m_insecure = !net::caBundleLoaded();
    }

    std::string title() const override { return {}; }

    bool handleInput(App& app) override
    {
        ui::Input& in = app.input();
        if (in.navigate(ui::Dir::Down)) app.focus().move(ui::Dir::Down);
        if (in.navigate(ui::Dir::Up)) app.focus().move(ui::Dir::Up);

        if (in.pressed(HidNpadButton_A) && !m_busy) {
            const ui::FocusId id = app.focus().current();
            if (id == fieldId(0)) {
                std::string out;
                if (util::textInput("Server address", m_url, out)) m_url = out;
            } else if (id == fieldId(1)) {
                m_insecure = !m_insecure;
                applyTls();
            } else if (id == fieldId(2)) {
                connect(app);
            }
        }
        if (in.pressed(HidNpadButton_Plus)) app.requestQuit();
        return true;
    }

    void draw(App& app) override
    {
        ui::Context& ctx = app.ui();
        const bool https = m_url.rfind("https://", 0) == 0;
        const int rows = https ? 3 : 2;
        const float height = 150.0f + rows * (kRowHeight + kRowGap) + 40.0f;
        const gfx::Rect card = centeredCard(app, height);

        ctx.r->fillRect(card, theme::Base200.withAlpha(0.92f), RadiusBox);

        const gfx::TexId logo = app.logo();
        if (logo != gfx::TexInvalid) {
            const float w = 190.0f;
            const float h = w * ctx.r->textureHeight(logo) / std::max(1u, ctx.r->textureWidth(logo));
            ctx.r->drawImage(logo, gfx::Rect{ card.cx() - w * 0.5f, card.y + 28, w, h },
                             theme::White);
        }

        gfx::TextStyle lead;
        lead.size = 16.0f;
        lead.color = theme::BaseContent.withAlpha(0.6f);
        ctx.text->drawAligned("Point this app at your Fliks server",
                              gfx::Rect{ card.x + 24, card.y + 104, card.w - 48, 24 }, lead,
                              ui::Align::Center);

        ctx.focus->beginContainer(0x5e40e401, false);
        float y = card.y + 140.0f;
        field(ctx, fieldId(0), gfx::Rect{ card.x + 24, y, card.w - 48, kRowHeight },
              "Server address", m_url, false);
        y += kRowHeight + kRowGap;

        if (https) {
            toggleRow(ctx, fieldId(1), gfx::Rect{ card.x + 24, y, card.w - 48, kRowHeight },
                      "Allow unverified TLS", m_insecure);
            y += kRowHeight + kRowGap;
        }

        ui::button(ctx, fieldId(2), gfx::Rect{ card.x + 24, y, card.w - 48, kRowHeight },
                   m_busy ? "Connecting…" : "Connect", ui::ButtonStyle::Primary);
        y += kRowHeight + 10.0f;
        ctx.focus->endContainer();

        if (m_busy)
            ui::spinner(ctx, gfx::Rect{ card.cx() - 12, y, 24, 24 }, theme::Primary, ctx.time);
        else if (!m_status.empty()) {
            gfx::TextStyle st;
            st.size = 14.0f;
            st.color = m_failed ? theme::Error : theme::BaseContent.withAlpha(0.6f);
            ctx.text->drawAligned(m_status, gfx::Rect{ card.x + 24, y, card.w - 48, 20 }, st,
                                  ui::Align::Center);
        }

        if (https && !net::caBundleLoaded() && !m_insecure) {
            gfx::TextStyle warn;
            warn.size = 13.0f;
            warn.color = theme::Warning;
            ctx.text->drawAligned("No CA bundle in romfs — run `make cacert` or allow unverified TLS",
                                  gfx::Rect{ card.x + 16, card.bottom() + 10, card.w - 32, 20 },
                                  warn, ui::Align::Center);
        }
    }

private:
    static ui::FocusId fieldId(uint32_t i) { return ui::focusId(10, i); }

    void applyTls() const
    {
        net::TlsOptions opts;
        opts.verifyPeer = !m_insecure;
        opts.caFile = "romfs:/cacert.pem";
        net::setTlsOptions(opts);
    }

    void connect(App& app)
    {
        std::string url = m_url;
        while (!url.empty() && url.back() == '/') url.pop_back();
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) url = "http://" + url;
        if (url.size() <= 8) {
            m_status = "Enter a server address";
            m_failed = true;
            return;
        }

        applyTls();
        app.client().setServer(url);
        m_url = url;
        m_busy = true;
        m_failed = false;
        m_status.clear();

        auto live = m_live;
        auto* tasks = &app.tasks();
        App* appPtr = &app;
        tasks->post(
            [this, live, tasks, appPtr, url] {
                net::Request req;
                req.url = url + "/api/auth/users-public";
                req.timeoutMs = 8000;
                const net::Response res = net::perform(req);
                const bool reachable = res.status > 0 && res.status < 500;
                std::string message = res.error;
                if (message.empty() && !reachable)
                    message = "HTTP " + std::to_string(res.status);

                tasks->postToMain([this, live, appPtr, reachable, message] {
                    if (!live->alive) return;
                    m_busy = false;
                    if (reachable) {
                        appPtr->client().saveSession();
                        appPtr->replace(makeLoginScreen());
                    } else {
                        m_failed = true;
                        m_status = message.empty() ? "Could not reach the server" : message;
                    }
                });
            },
            util::TaskQueue::Priority::High);
    }

    std::shared_ptr<Liveness> m_live;
    std::string m_url;
    std::string m_status;
    bool m_insecure = false;
    bool m_busy = false;
    bool m_failed = false;
};

// ---------------------------------------------------------------------------

class LoginScreen final : public Screen {
public:
    LoginScreen() : m_live(std::make_shared<Liveness>()) {}
    ~LoginScreen() override { m_live->alive = false; }

    void onEnter(App& app) override { fetchUsers(app); }

    bool handleInput(App& app) override
    {
        ui::Input& in = app.input();
        if (in.navigate(ui::Dir::Down)) app.focus().move(ui::Dir::Down);
        if (in.navigate(ui::Dir::Up)) app.focus().move(ui::Dir::Up);
        if (in.navigate(ui::Dir::Left)) app.focus().move(ui::Dir::Left);
        if (in.navigate(ui::Dir::Right)) app.focus().move(ui::Dir::Right);

        if (in.pressed(HidNpadButton_B)) {
            app.replace(makeServerScreen());
            return true;
        }
        if (in.pressed(HidNpadButton_A) && !m_busy) {
            const ui::FocusId id = app.focus().current();
            if (id == rowId(0)) {
                std::string out;
                if (util::textInput("Username", m_username, out)) m_username = out;
            } else if (id == rowId(1)) {
                std::string out;
                if (util::textInput("Password", "", out, true)) m_password = out;
            } else if (id == rowId(2)) {
                signIn(app);
            } else {
                for (size_t i = 0; i < m_users.size(); i++) {
                    if (id == userId(static_cast<uint32_t>(i))) {
                        m_username = m_users[i];
                        app.focus().setCurrent(rowId(1));
                        std::string out;
                        if (util::textInput("Password", "", out, true)) {
                            m_password = out;
                            signIn(app);
                        }
                        break;
                    }
                }
            }
        }
        return true;
    }

    void draw(App& app) override
    {
        ui::Context& ctx = app.ui();
        const bool hasUsers = !m_users.empty();
        const float usersHeight = hasUsers ? 96.0f : 0.0f;
        const float height = 150.0f + usersHeight + 3 * (kRowHeight + kRowGap) + 36.0f;
        const gfx::Rect card = centeredCard(app, height);

        ctx.r->fillRect(card, theme::Base200.withAlpha(0.92f), RadiusBox);

        const gfx::TexId logo = app.logo();
        if (logo != gfx::TexInvalid) {
            const float w = 190.0f;
            const float h = w * ctx.r->textureHeight(logo) / std::max(1u, ctx.r->textureWidth(logo));
            ctx.r->drawImage(logo, gfx::Rect{ card.cx() - w * 0.5f, card.y + 28, w, h },
                             theme::White);
        }

        gfx::TextStyle lead;
        lead.size = 16.0f;
        lead.color = theme::BaseContent.withAlpha(0.6f);
        ctx.text->drawAligned("Sign in", gfx::Rect{ card.x + 24, card.y + 104, card.w - 48, 24 },
                              lead, ui::Align::Center);

        ctx.focus->beginContainer(0x5e40e402, false);
        float y = card.y + 140.0f;

        if (hasUsers) {
            ctx.focus->beginContainer(0x5e40e403, true);
            float x = card.x + 24;
            for (size_t i = 0; i < m_users.size() && x < card.right() - 90; i++) {
                const gfx::Rect chip{ x, y, 84.0f, 84.0f };
                const ui::FocusId id = userId(static_cast<uint32_t>(i));
                const bool focused = ctx.focus->isFocused(id);
                ctx.focus->add(id, chip);
                if (focused) ui::focusRing(ctx, chip, RadiusBox);
                ctx.r->fillRect(chip, theme::Base300, RadiusBox);
                ctx.icons->draw(*ctx.r, ui::Icon::User,
                                gfx::Rect{ chip.cx() - 16, chip.y + 14, 32, 32 },
                                theme::BaseContent.withAlpha(0.7f));
                gfx::TextStyle name;
                name.size = 13.0f;
                name.color = theme::BaseContent.withAlpha(0.8f);
                ctx.text->drawEllipsized(m_users[i],
                                         gfx::Rect{ chip.x + 4, chip.bottom() - 24, chip.w - 8, 18 },
                                         name, ui::Align::Center);
                x += 92.0f;
            }
            ctx.focus->endContainer();
            y += 96.0f;
        }

        field(ctx, rowId(0), gfx::Rect{ card.x + 24, y, card.w - 48, kRowHeight }, "Username",
              m_username, false);
        y += kRowHeight + kRowGap;
        field(ctx, rowId(1), gfx::Rect{ card.x + 24, y, card.w - 48, kRowHeight }, "Password",
              m_password, true);
        y += kRowHeight + kRowGap;
        ui::button(ctx, rowId(2), gfx::Rect{ card.x + 24, y, card.w - 48, kRowHeight },
                   m_busy ? "Signing in…" : "Sign in", ui::ButtonStyle::Primary);
        y += kRowHeight + 8.0f;
        ctx.focus->endContainer();

        if (!m_error.empty()) {
            gfx::TextStyle st;
            st.size = 14.0f;
            st.color = theme::Error;
            ctx.text->drawAligned(m_error, gfx::Rect{ card.x + 24, y, card.w - 48, 20 }, st,
                                  ui::Align::Center);
        }

        gfx::TextStyle hint;
        hint.size = 13.0f;
        hint.color = theme::BaseContent.withAlpha(0.4f);
        ctx.text->drawAligned(app.client().server(),
                              gfx::Rect{ card.x, card.bottom() + 12, card.w, 18 }, hint,
                              ui::Align::Center);
    }

private:
    static ui::FocusId rowId(uint32_t i) { return ui::focusId(11, i); }
    static ui::FocusId userId(uint32_t i) { return ui::focusId(12, i); }

    void fetchUsers(App& app)
    {
        auto live = m_live;
        auto* tasks = &app.tasks();
        const std::string url = app.client().server() + "/api/auth/users-public";
        tasks->post([this, live, tasks, url] {
            net::Request req;
            req.url = url;
            const net::Response res = net::perform(req);
            std::vector<std::string> names;
            if (res.ok()) {
                util::Json arr = util::Json::parse(res.body);
                for (size_t i = 0; i < arr.size(); i++) names.push_back(arr.at(i)["username"].str());
            }
            tasks->postToMain([this, live, names] {
                if (!live->alive) return;
                m_users = names;
            });
        });
    }

    void signIn(App& app)
    {
        if (m_username.empty() || m_password.empty()) {
            m_error = "Enter a username and password";
            return;
        }
        m_busy = true;
        m_error.clear();

        auto live = m_live;
        auto* tasks = &app.tasks();
        App* appPtr = &app;
        const std::string user = m_username;
        const std::string pass = m_password;

        tasks->post(
            [this, live, tasks, appPtr, user, pass] {
                std::string error;
                const bool ok = appPtr->client().login(user, pass, error);
                tasks->postToMain([this, live, appPtr, ok, error] {
                    if (!live->alive) return;
                    m_busy = false;
                    m_password.clear();
                    if (ok) appPtr->replace(makeHomeScreen());
                    else m_error = error.empty() ? "Sign-in failed" : error;
                });
            },
            util::TaskQueue::Priority::High);
    }

    std::shared_ptr<Liveness> m_live;
    std::vector<std::string> m_users;
    std::string m_username;
    std::string m_password;
    std::string m_error;
    bool m_busy = false;
};

} // namespace

std::unique_ptr<Screen> makeServerScreen() { return std::make_unique<ServerScreen>(); }
std::unique_ptr<Screen> makeLoginScreen() { return std::make_unique<LoginScreen>(); }

} // namespace app
