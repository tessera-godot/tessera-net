// SPDX-License-Identifier: Apache-2.0
// The example server on a real UDP socket in loopback: a client connects, sees the entities, the state
// converges, and the server stops cleanly.
#include "example_game.hpp"
#include "tessera/net/client_replica.hpp"
#include "tessera/net/transport/enet_transport.hpp"
#include "tessera/server/server_host.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace tessera::net;
namespace server = tessera::server;

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kLimit = std::chrono::seconds(10);

// The example server and a client of it, with the server on a thread of its own as a real program has it.
struct Running {
    Running(uint32_t entities, bool walking)
        : game(entities, walking), transport(Listen()),
          log(server::LogLevel::Info,
              [this](const std::string& line) {
                  std::lock_guard<std::mutex> lock(lines_mutex);
                  lines.push_back(line);
              }),
          host(Config(), game, transport, log, [this] { transport.Flush(); }),
          client_transport(Dial(transport.Port())), client(client_transport, *game.DescribeWorld().schema) {
        runner = std::thread([this] { code = host.Run(); });
    }

    ~Running() {
        Stop();
    }

    // Asks the server to stop and waits for it; returns the exit code of `Run`.
    int Stop() {
        host.RequestStop();
        if (runner.joinable()) {
            runner.join();
        }
        return code;
    }

    // Reads the client until all the entities are visible with a state, or the limit passes.
    bool WaitForAll(uint32_t entities) {
        const auto deadline = Clock::now() + kLimit;
        while (Clock::now() < deadline) {
            client.Poll();
            client_transport.Flush();
            if (client.VisibleCount() == entities && CountWithState() == entities) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    uint32_t CountWithState() const {
        uint32_t count = 0;
        client.ForEachEntity([&count](EntityId, const uint32_t*) { ++count; });
        return count;
    }

    // The x word of every entity the client has, by the index in the entity's id.
    std::map<uint32_t, uint32_t> XWords() const {
        std::map<uint32_t, uint32_t> words;
        client.ForEachEntity([&words](EntityId id, const uint32_t* state) { words[id.index()] = state[0]; });
        return words;
    }

    bool LogHas(const std::string& text) {
        std::lock_guard<std::mutex> lock(lines_mutex);
        for (const std::string& line : lines) {
            if (line.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    static EnetServerConfig Listen() {
        EnetServerConfig listen;
        listen.bind_address = "127.0.0.1";
        listen.port = 0; // the system chooses; `Port()` says which
        listen.max_clients = 8;
        return listen;
    }
    static EnetClientConfig Dial(uint16_t port) {
        EnetClientConfig dial;
        dial.server_address = "127.0.0.1";
        dial.server_port = port;
        return dial;
    }
    static server::ServerConfig Config() {
        server::ServerConfig config;
        config.max_players = 8;
        config.tick_rate_hz = 50;
        return config;
    }

    server::example::ExampleGame game;
    EnetServerTransport transport;
    std::mutex lines_mutex;
    std::vector<std::string> lines;
    server::Logger log;
    server::ServerHost host;
    EnetClientTransport client_transport;
    ClientReplica client;
    std::thread runner;
    int code = -1;
};

} // namespace

TESSERA_TEST(example_server_converges_over_enet_and_stops_cleanly) {
    constexpr uint32_t kEntities = 20;
    Running running(kEntities, false);
    const bool converged = running.WaitForAll(kEntities);
    TESSERA_CHECK(converged);
    TESSERA_CHECK(running.client.State() == ClientState::Connected);

    // The state the client holds is what the game made: each x is the start position, in whole steps of a
    // centimeter from the edge of the range.
    const std::map<uint32_t, uint32_t> words = running.XWords();
    TESSERA_CHECK(words.size() == kEntities);
    for (const auto& [index, word] : words) {
        const auto [x, z] = server::example::ExampleGame::InitialPosition(index);
        (void)z;
        using Game = server::example::ExampleGame;
        const double offset = static_cast<double>(x) + static_cast<double>(Game::kPositionRange);
        const long long expected = std::llround(offset / static_cast<double>(Game::kPositionStep));
        TESSERA_CHECK(static_cast<long long>(word) == expected);
    }

    const int code = running.Stop();
    TESSERA_CHECK(code == 0);
    TESSERA_CHECK(running.LogHas("example game stopped"));
    TESSERA_CHECK(running.LogHas("INFO  stopped"));
}

TESSERA_TEST(example_entities_keep_moving_when_walking) {
    constexpr uint32_t kEntities = 20;
    Running running(kEntities, true);
    TESSERA_CHECK(running.WaitForAll(kEntities));
    const std::map<uint32_t, uint32_t> before = running.XWords();
    const auto until = Clock::now() + std::chrono::milliseconds(300);
    while (Clock::now() < until) {
        running.client.Poll();
        running.client_transport.Flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const std::map<uint32_t, uint32_t> after = running.XWords();
    TESSERA_CHECK(after.size() == kEntities && before.size() == kEntities);
    TESSERA_CHECK(before != after); // at up to 5 m/s, in 0.3 s an entity moves by hundreds of steps
    TESSERA_CHECK(running.Stop() == 0);
}
