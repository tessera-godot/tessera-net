// SPDX-License-Identifier: Apache-2.0
// The stop signals of the example server, with real signals sent to this process. Windows has no signal to
// send to oneself here; the process test (run_example_process.py) covers Ctrl+Break there.
#include "example_game.hpp"
#include "stop_signals.hpp"
#include "tessera/net/transport/simulated_network.hpp"
#include "tessera/server/server_host.hpp"
#include "test_support.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#ifndef _WIN32
#include <csignal>
#endif

using namespace tessera::net;
namespace server = tessera::server;

#ifndef _WIN32

namespace {

struct Rig {
    Rig()
        : network(NetworkConditions{}), game(4, false), log(server::LogLevel::Error, [](const std::string&) {}) {
        server::ServerConfig config;
        config.max_players = 8;
        config.tick_rate_hz = 100;
        host = std::make_unique<server::ServerHost>(config, game, network.Server(), log);
    }

    SimulatedNetwork network;
    server::example::ExampleGame game;
    server::Logger log;
    std::unique_ptr<server::ServerHost> host;
};

} // namespace

TESSERA_TEST(stop_signal_stops_a_running_host_with_code_0) {
    Rig rig;
    server::example::InstallStopSignals(*rig.host);
    int code = -1;
    std::thread runner([&] { code = rig.host->Run(); });
    while (rig.host->TickCount() < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::raise(SIGTERM);
    runner.join();
    server::example::RemoveStopSignals();
    TESSERA_CHECK(code == 0);
    TESSERA_CHECK(rig.host->StopRequested());
}

TESSERA_TEST(a_signal_after_the_signals_were_removed_does_not_end_the_process) {
    {
        Rig rig;
        server::example::InstallStopSignals(*rig.host);
        server::example::RemoveStopSignals();
    } // the host is gone, as it is when main leaves; a late signal must find nothing to call
    std::raise(SIGTERM);
    std::raise(SIGINT);
    TESSERA_CHECK(true); // still here: the default action would have ended the process
}

TESSERA_TEST(signals_can_be_installed_again_for_another_host) {
    for (int round = 0; round < 2; ++round) {
        Rig rig;
        server::example::InstallStopSignals(*rig.host);
        int code = -1;
        std::thread runner([&] { code = rig.host->Run(); });
        std::raise(SIGINT);
        runner.join();
        server::example::RemoveStopSignals();
        TESSERA_CHECK(code == 0);
    }
}

#endif
