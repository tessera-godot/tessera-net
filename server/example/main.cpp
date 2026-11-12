// SPDX-License-Identifier: Apache-2.0
// The example server: a small game on a real UDP port. Run `tessera_example_server --help` for the settings.
// Your own server has the same shape: read the configuration, open a transport, build a `ServerHost` around
// your game, and run it.
#include "example_game.hpp"
#include "stop_signals.hpp"
#include "tessera/net/transport/enet_transport.hpp"
#include "tessera/server/log.hpp"
#include "tessera/server/server_config.hpp"
#include "tessera/server/server_host.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

using namespace tessera::server;

namespace {

// The most players ENet can serve at once.
constexpr uint32_t kMostPlayers = 4095;

// Passes the stop signals to a host for as long as it lives; the signals are removed on every way out of the
// scope, an exception included, before the host goes away.
class StopSignalsScope {
public:
    explicit StopSignalsScope(ServerHost& host) {
        example::InstallStopSignals(host);
    }
    ~StopSignalsScope() {
        example::RemoveStopSignals();
    }
    StopSignalsScope(const StopSignalsScope&) = delete;
    StopSignalsScope& operator=(const StopSignalsScope&) = delete;
};

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    for (const std::string_view arg : args) {
        if (arg == "--help" || arg == "-h") {
            std::fputs(ConfigHelp().c_str(), stdout);
            return 0;
        }
    }

    ServerConfig config;
    try {
        config = ResolveConfig(args);
        if (config.max_players > kMostPlayers) {
            throw ConfigError("max_players",
                              "the ENet transport takes at most " + std::to_string(kMostPlayers));
        }
    } catch (const ConfigError& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 2;
    }

    try {
        Logger log(config.log_level, StdoutSink());

        tessera::net::EnetServerConfig listen;
        listen.bind_address = ""; // every interface
        listen.port = config.port;
        listen.max_clients = config.max_players;
        tessera::net::EnetServerTransport transport(listen);

        example::ExampleGame game;
        ServerHost host(config, game, transport, log, [&transport] { transport.Flush(); });
        const StopSignalsScope signals(host);

        log.Info("listening on port " + std::to_string(transport.Port()));
        return host.Run();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
