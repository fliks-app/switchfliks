#include <switch.h>

#include "app/App.h"
#include "net/Http.h"
#include "player/Player.h"
#include "util/Config.h"
#include "util/Log.h"

namespace {

// A generous socket pool: the image loader keeps several transfers in flight
// while the player holds its own connection open for the whole film.
const SocketInitConfig kSocketConfig = [] {
    SocketInitConfig config = *socketGetDefaultInitConfig();
    config.tcp_tx_buf_size = 1 * 1024 * 1024;
    config.tcp_rx_buf_size = 1 * 1024 * 1024;
    config.tcp_tx_buf_max_size = 4 * 1024 * 1024;
    config.tcp_rx_buf_max_size = 4 * 1024 * 1024;
    config.num_bsd_sessions = 8;
    return config;
}();

} // namespace

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    const Result romfsRc = romfsInit();
    util::logInit();
    FLIKS_LOG("romfsInit: 0x%x", romfsRc);
    FLIKS_LOG("socketInitialize: 0x%x", socketInitialize(&kSocketConfig));
    FLIKS_LOG("plInitialize: 0x%x", plInitialize(PlServiceType_User));
    util::loadConfig();
    player::loadSettings();
    net::globalInit();

    // Verification is on wherever a bundle is available; without one the
    // server screen surfaces the state and offers the explicit opt-out
    // rather than quietly trusting whatever answers.
    net::TlsOptions tls;
    tls.verifyPeer = true;
    tls.caFile = "romfs:/cacert.pem";
    net::setTlsOptions(tls);

    {
        app::App application;
        if (application.init()) {
            application.run();
        } else {
            FLIKS_LOG("startup aborted");
        }
        application.shutdown();
    }

    FLIKS_LOG("exiting");
    util::logShutdown();
    net::globalShutdown();
    plExit();
    socketExit();
    romfsExit();
    return 0;
}
