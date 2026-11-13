// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/client_replica.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/transport/simulated_network.hpp"
#include "tessera/server/server_host.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace tessera::net;
using namespace tessera::server;

namespace {

struct MobColumns {
    float x[kPageSlots];
    float y[kPageSlots];
    float z[kPageSlots];
};

// One schema for the whole file: a host keeps a pointer to the schema its game describes.
const ReplicationSchema& TestSchema() {
    static const ReplicationSchema schema = [] {
        ReplicationSchema built;
        built.AddType(1, {PropertyDescriptor::Position(TESSERA_COLUMN(MobColumns, x),
                                                       TESSERA_COLUMN(MobColumns, y),
                                                       TESSERA_COLUMN(MobColumns, z), 0.01f, 5000.0f)});
        return built;
    }();
    return schema;
}

size_t Count(const std::vector<std::string>& events, const std::string& what) {
    return static_cast<size_t>(std::count(events.begin(), events.end(), what));
}

// Where in `events` a thing happened first, or the size if it did not.
size_t IndexOf(const std::vector<std::string>& events, const std::string& what) {
    return static_cast<size_t>(std::find(events.begin(), events.end(), what) - events.begin());
}

// A game that writes down what the host asks of it, and can run a function at each call.
class RecordingLogic final : public IGameLogic {
public:
    std::vector<std::string> events;
    std::vector<uint8_t> last_input_bytes;
    bool make_entity = false; // one entity that every player sees, made in OnStart
    EntityId entity = EntityId::Null();
    uint32_t seen_shard = 0xFFFFFFFF;
    uint32_t seen_tick_rate = 0;
    double last_dt = 0.0;
    uint64_t ticks = 0;
    std::function<void()> on_tick_hook;
    std::function<void(ConnectionId)> on_connected_hook;
    std::function<void()> on_shutdown_hook;
    std::function<void()> on_flush_hook;
    std::function<void()> on_start_hook;

    WorldSetup DescribeWorld() override {
        WorldSetup setup;
        setup.schema = &TestSchema();
        setup.relevancy.types[1] = TypeRelevancy{RelevancyMode::AlwaysRelevant, 0.0f, 0.0f};
        return setup;
    }
    void OnStart(ServerContext& context) override {
        events.push_back("start");
        if (on_start_hook) {
            on_start_hook();
        }
        context_ = &context;
        seen_shard = context.shard_id;
        seen_tick_rate = context.tick_rate_hz;
        if (make_entity) {
            table_ = std::make_unique<EntityTable<MobColumns>>(TableConfig{context.shard_id, 1});
            entity = table_->Create();
            const auto where = table_->Find(entity);
            MobColumns& columns = table_->Columns(where->page);
            columns.x[where->slot] = 10.0f;
            columns.y[where->slot] = 0.0f;
            columns.z[where->slot] = 20.0f;
            table_->MarkDirty(entity);
            context.grid.Insert(entity, 10.0f, 0.0f, 20.0f);
            context.replication.Relevancy().OnEntityCreated(entity);
        }
    }
    void OnTick(double dt_seconds) override {
        events.push_back("tick");
        last_dt = dt_seconds;
        ++ticks;
        if (table_) {
            context_->replication.Refresh(*table_);
        }
        if (on_tick_hook) {
            on_tick_hook();
        }
    }
    void OnPlayerConnected(ConnectionId player) override {
        events.push_back("connected:" + std::to_string(player));
        if (on_connected_hook) {
            on_connected_hook(player);
        }
    }
    void OnPlayerDisconnected(ConnectionId player) override {
        events.push_back("disconnected:" + std::to_string(player));
    }
    void OnPlayerInput(ConnectionId player, uint32_t sequence, std::span<const uint8_t> bytes) override {
        events.push_back("input:" + std::to_string(player) + ":" + std::to_string(sequence));
        last_input_bytes.assign(bytes.begin(), bytes.end());
    }
    void OnShutdown() override {
        events.push_back("shutdown");
        if (on_shutdown_hook) {
            on_shutdown_hook();
        }
    }

private:
    ServerContext* context_ = nullptr;
    std::unique_ptr<EntityTable<MobColumns>> table_;
};

// A host over a simulated network, with a game that records, and the log lines it wrote.
struct Rig {
    explicit Rig(uint32_t shard_id = 0, uint32_t tick_rate_hz = 100, bool entity = false)
        : network(NetworkConditions{}), log(LogLevel::Debug, [this](const std::string& line) {
              lines.push_back(line);
          }) {
        logic.make_entity = entity;
        config.shard_id = shard_id;
        config.tick_rate_hz = tick_rate_hz;
        config.max_players = 8;
        host = std::make_unique<ServerHost>(config, logic, network.Server(), log,
                                            [this] {
                                                logic.events.push_back("flush");
                                                if (logic.on_flush_hook) {
                                                    logic.on_flush_hook();
                                                }
                                            });
    }

    // One tick the way a game loop would run it: the network moves, the clients read, the host steps.
    void Step(std::initializer_list<ClientReplica*> clients = {}) {
        network.Advance(++now);
        for (ClientReplica* client : clients) {
            client->Poll();
        }
        host->Step();
    }

    RecordingLogic logic;
    SimulatedNetwork network;
    std::vector<std::string> lines;
    Logger log;
    tessera::server::ServerConfig config; // spelled out: `net` has a `ServerConfig` too
    std::unique_ptr<ServerHost> host;
    uint64_t now = 0;
};

// Stops a host whose `Run` nothing else is going to stop, so that a bug fails a test instead of hanging the
// whole run. The limit is far longer than any test needs.
class Watchdog {
public:
    explicit Watchdog(ServerHost& host, std::chrono::milliseconds limit = std::chrono::seconds(5))
        : thread_([this, &host, limit] {
              const auto deadline = std::chrono::steady_clock::now() + limit;
              while (!done_.load() && std::chrono::steady_clock::now() < deadline) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(10));
              }
              if (!done_.load()) {
                  fired_.store(true);
                  host.RequestStop();
              }
          }) {}
    ~Watchdog() {
        done_.store(true);
        thread_.join();
    }
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    bool Fired() const {
        return fired_.load();
    }

private:
    std::atomic<bool> done_{false};
    std::atomic<bool> fired_{false};
    std::thread thread_;
};

} // namespace

TESSERA_TEST(host_start_calls_on_start_once_with_the_shard_and_tick_rate) {
    Rig rig(9, 50);
    TESSERA_CHECK(rig.logic.events.empty()); // building the host starts nothing
    rig.host->Start();
    rig.host->Start();
    TESSERA_CHECK(rig.logic.events == std::vector<std::string>{"start"});
    TESSERA_CHECK(rig.logic.seen_shard == 9);
    TESSERA_CHECK(rig.logic.seen_tick_rate == 50);
}

TESSERA_TEST(host_step_calls_on_tick_with_the_fixed_dt) {
    Rig rig(0, 50);
    rig.host->Step();
    rig.host->Step();
    TESSERA_CHECK(rig.logic.ticks == 2);
    TESSERA_CHECK(std::abs(rig.logic.last_dt - 0.02) < 1e-9);
    TESSERA_CHECK(rig.host->TickCount() == 2);
    // The first Step started the game; OnStart came before the first OnTick.
    TESSERA_CHECK(rig.logic.events.size() >= 2 && rig.logic.events[0] == "start" &&
                  rig.logic.events[1] == "tick");
}

TESSERA_TEST(host_run_never_ticks_faster_than_the_rate) {
    Rig rig(0, 100); // 10 ms per tick
    rig.logic.on_tick_hook = [&] {
        if (rig.logic.ticks == 10) {
            rig.host->RequestStop();
        }
    };
    const auto start = std::chrono::steady_clock::now();
    Watchdog guard(*rig.host);
    const int code = rig.host->Run();
    TESSERA_CHECK(!guard.Fired());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    TESSERA_CHECK(code == 0);
    TESSERA_CHECK(rig.logic.ticks == 10);
    TESSERA_CHECK(elapsed >= std::chrono::milliseconds(9 * 10)); // nine gaps between ten ticks
    TESSERA_CHECK(elapsed < std::chrono::seconds(2));            // and it does not crawl either
}

TESSERA_TEST(host_player_callbacks_arrive_in_order) {
    Rig rig;
    ClientReplica first(*rig.network.AddClient(), TestSchema());
    ClientReplica second(*rig.network.AddClient(), TestSchema());
    for (int i = 0; i < 8; ++i) {
        rig.Step({&first, &second});
    }
    TESSERA_CHECK(first.State() == ClientState::Connected && second.State() == ClientState::Connected);
    TESSERA_CHECK(rig.host->Replication().ConnectionCount() == 2);
    first.Disconnect();
    for (int i = 0; i < 4; ++i) {
        rig.Step({&second});
    }
    const auto& events = rig.logic.events;
    TESSERA_CHECK(Count(events, "connected:0") == 1 && Count(events, "connected:1") == 1);
    TESSERA_CHECK(Count(events, "disconnected:0") == 1);
    TESSERA_CHECK(IndexOf(events, "connected:0") < IndexOf(events, "connected:1"));
    TESSERA_CHECK(IndexOf(events, "connected:1") < IndexOf(events, "disconnected:0"));
    TESSERA_CHECK(rig.host->Replication().ConnectionCount() == 1);
}

TESSERA_TEST(host_hands_inputs_to_the_game_in_order_with_the_player_id) {
    Rig rig;
    ClientReplica client(*rig.network.AddClient(), TestSchema());
    for (int i = 0; i < 8 && client.State() != ClientState::Connected; ++i) {
        rig.Step({&client});
    }
    TESSERA_CHECK(client.State() == ClientState::Connected);
    for (const uint8_t byte : {uint8_t{11}, uint8_t{12}, uint8_t{13}}) {
        client.SendInput(std::span<const uint8_t>(&byte, 1));
        rig.Step({&client});
    }
    rig.Step({&client});
    const auto& events = rig.logic.events;
    TESSERA_CHECK(Count(events, "input:0:1") == 1 && Count(events, "input:0:2") == 1 &&
                  Count(events, "input:0:3") == 1);
    TESSERA_CHECK(IndexOf(events, "connected:0") < IndexOf(events, "input:0:1"));
    TESSERA_CHECK(IndexOf(events, "input:0:1") < IndexOf(events, "input:0:2"));
    TESSERA_CHECK(IndexOf(events, "input:0:2") < IndexOf(events, "input:0:3"));
    TESSERA_CHECK((rig.logic.last_input_bytes == std::vector<uint8_t>{13}));
}

TESSERA_TEST(host_input_never_arrives_after_the_disconnect_callback) {
    {
        Rig rig;
        ClientReplica client(*rig.network.AddClient(), TestSchema());
        for (int i = 0; i < 8 && client.State() != ClientState::Connected; ++i) {
            rig.Step({&client});
        }
        const uint8_t byte = 5;
        TESSERA_CHECK(client.SendInput(std::span<const uint8_t>(&byte, 1)) == 1);
        client.Disconnect(); // in the same step
        rig.Step();
        rig.Step();
        const auto& events = rig.logic.events;
        TESSERA_CHECK(Count(events, "input:0:1") == 1 && Count(events, "disconnected:0") == 1);
        TESSERA_CHECK(IndexOf(events, "input:0:1") < IndexOf(events, "disconnected:0"));
    }
    {
        // A client that breaks the protocol is dropped; an input it sent after that goes nowhere.
        Rig rig;
        ITransport* raw = rig.network.AddClient();
        rig.network.Advance(++rig.now);
        std::vector<uint8_t> bytes;
        Encode(ClientHello{kProtocolVersion, TestSchema().Hash()}, bytes);
        raw->Send(0, Channel::ReliableOrdered, bytes);
        rig.Step();
        rig.Step();
        TESSERA_CHECK(Count(rig.logic.events, "connected:0") == 1);
        raw->Send(0, Channel::ReliableOrdered, std::vector<uint8_t>{0xFF}); // not a message
        Input input;
        InputEntry entry;
        entry.sequence = 1;
        input.entries.push_back(entry);
        bytes.clear();
        Encode(input, bytes);
        raw->Send(0, Channel::Unreliable, bytes);
        rig.Step();
        rig.Step();
        const auto& events = rig.logic.events;
        TESSERA_CHECK(Count(events, "disconnected:0") == 1 && Count(events, "input:0:1") == 0);
    }
}

TESSERA_TEST(host_a_game_that_does_not_override_on_player_input_just_works) {
    class PlainLogic final : public IGameLogic {
    public:
        WorldSetup DescribeWorld() override {
            WorldSetup setup;
            setup.schema = &TestSchema();
            setup.relevancy.types[1] = TypeRelevancy{RelevancyMode::AlwaysRelevant, 0.0f, 0.0f};
            return setup;
        }
        void OnTick(double) override {}
    };
    PlainLogic logic;
    SimulatedNetwork network{NetworkConditions{}};
    Logger log(LogLevel::Debug, [](const std::string&) {});
    tessera::server::ServerConfig config;
    config.max_players = 8;
    ServerHost host(config, logic, network.Server(), log);
    ClientReplica client(*network.AddClient(), TestSchema());
    uint64_t now = 0;
    auto step = [&] {
        network.Advance(++now);
        client.Poll();
        host.Step();
    };
    for (int i = 0; i < 8 && client.State() != ClientState::Connected; ++i) {
        step();
    }
    TESSERA_CHECK(client.State() == ClientState::Connected);
    const uint8_t byte = 9;
    TESSERA_CHECK(client.SendInput(std::span<const uint8_t>(&byte, 1)) == 1);
    step();
    step();
    TESSERA_CHECK(host.Replication().Stats().inputs_delivered == 1);
    TESSERA_CHECK(host.Replication().ConnectionCount() == 1);
}

TESSERA_TEST(host_client_that_leaves_before_hello_gets_no_callbacks) {
    Rig rig;
    ITransport* raw = rig.network.AddClient();
    rig.network.Advance(++rig.now); // both ends see Connected; the client never reads it, so no hello
    raw->Disconnect(0); // a client sees the server as connection 0
    rig.Step();                     // the server reads the connection and its end in one go
    TESSERA_CHECK(Count(rig.logic.events, "connected:0") == 0);
    TESSERA_CHECK(Count(rig.logic.events, "disconnected:0") == 0);
    TESSERA_CHECK(rig.host->Replication().ConnectionCount() == 0);
    rig.Step();
    TESSERA_CHECK(rig.host->Replication().ConnectionCount() == 0);
}

TESSERA_TEST(host_client_that_says_hello_and_leaves_in_one_tick_gets_both_callbacks_in_order) {
    Rig rig;
    ClientReplica client(*rig.network.AddClient(), TestSchema());
    rig.network.Advance(++rig.now);
    client.Poll();       // sees Connected and sends the hello
    client.Disconnect(); // and leaves before the server has read anything
    rig.Step();
    const auto& events = rig.logic.events;
    TESSERA_CHECK(Count(events, "connected:0") == 1);
    TESSERA_CHECK(Count(events, "disconnected:0") == 1);
    TESSERA_CHECK(IndexOf(events, "connected:0") < IndexOf(events, "disconnected:0"));
    TESSERA_CHECK(rig.host->Replication().ConnectionCount() == 0);
    rig.Step();
    TESSERA_CHECK(Count(events, "connected:0") == 1 && Count(events, "disconnected:0") == 1);
}

TESSERA_TEST(host_entities_carry_the_shard_id_to_the_client) {
    Rig rig(7, 100, true);
    ClientReplica client(*rig.network.AddClient(), TestSchema());
    for (int i = 0; i < 10; ++i) {
        rig.Step({&client});
    }
    TESSERA_CHECK(client.State() == ClientState::Connected);
    TESSERA_CHECK(client.VisibleCount() == 1);
    TESSERA_CHECK(client.WordsOf(rig.logic.entity) != nullptr);
    TESSERA_CHECK(rig.logic.entity.shard() == 7);
}

TESSERA_TEST(host_accepts_shard_id_4095) {
    Rig rig(4095, 100, true);
    ClientReplica client(*rig.network.AddClient(), TestSchema());
    for (int i = 0; i < 10; ++i) {
        rig.Step({&client});
    }
    TESSERA_CHECK(rig.logic.entity.shard() == 4095);
    TESSERA_CHECK(client.WordsOf(rig.logic.entity) != nullptr);
}

TESSERA_TEST(host_two_hosts_in_one_process_do_not_share_state) {
    Rig first(1, 100, true);
    Rig second(2, 100, true);
    ClientReplica client(*first.network.AddClient(), TestSchema());
    for (int i = 0; i < 10; ++i) {
        first.Step({&client});
    }
    for (int i = 0; i < 4; ++i) {
        second.Step();
    }
    TESSERA_CHECK(first.logic.entity.shard() == 1 && second.logic.entity.shard() == 2);
    TESSERA_CHECK(first.host->TickCount() == 10 && second.host->TickCount() == 4);
    TESSERA_CHECK(first.host->Replication().ConnectionCount() == 1);
    TESSERA_CHECK(second.host->Replication().ConnectionCount() == 0);
    TESSERA_CHECK(client.WordsOf(first.logic.entity) != nullptr);
    TESSERA_CHECK(first.logic.seen_shard == 1 && second.logic.seen_shard == 2);
}

TESSERA_TEST(host_stop_calls_on_shutdown_once_and_returns_0) {
    Rig rig;
    rig.logic.on_tick_hook = [&] {
        if (rig.logic.ticks == 3) {
            rig.host->RequestStop();
        }
    };
    TESSERA_CHECK(!rig.host->StopRequested());
    Watchdog guard(*rig.host);
    const int code = rig.host->Run();
    TESSERA_CHECK(!guard.Fired());
    TESSERA_CHECK(code == 0);
    TESSERA_CHECK(rig.host->StopRequested());
    TESSERA_CHECK(Count(rig.logic.events, "start") == 1);
    TESSERA_CHECK(Count(rig.logic.events, "shutdown") == 1);
    TESSERA_CHECK(rig.logic.ticks == 3);
    TESSERA_CHECK(rig.logic.events.back() == "shutdown"); // nothing after it
}

TESSERA_TEST(host_second_stop_request_does_not_repeat_anything) {
    Rig rig;
    rig.logic.on_tick_hook = [&] { rig.host->RequestStop(); };
    Watchdog guard(*rig.host);
    TESSERA_CHECK(rig.host->Run() == 0);
    TESSERA_CHECK(!guard.Fired());
    const std::vector<std::string> after_run = rig.logic.events;
    rig.host->RequestStop(); // a second request, as a second Ctrl+C would make
    rig.host->Shutdown();
    TESSERA_CHECK(rig.host->Run() == 0); // already shut down: returns at once, ticks nothing
    rig.host->Step();
    TESSERA_CHECK(rig.logic.events == after_run);
    TESSERA_CHECK(Count(rig.logic.events, "shutdown") == 1);
}

TESSERA_TEST(host_stop_before_run_starts_no_tick) {
    Rig rig;
    rig.host->RequestStop(); // a signal that arrives while the program is still starting
    Watchdog guard(*rig.host);
    const int code = rig.host->Run();
    TESSERA_CHECK(!guard.Fired());
    TESSERA_CHECK(code == 0);
    TESSERA_CHECK(Count(rig.logic.events, "start") == 1);
    TESSERA_CHECK(Count(rig.logic.events, "shutdown") == 1);
    TESSERA_CHECK(Count(rig.logic.events, "tick") == 0);
    TESSERA_CHECK(rig.host->TickCount() == 0);
    TESSERA_CHECK(IndexOf(rig.logic.events, "start") < IndexOf(rig.logic.events, "shutdown"));
}

TESSERA_TEST(host_stop_inside_on_tick_finishes_that_tick) {
    Rig rig;
    rig.logic.on_tick_hook = [&] { rig.host->RequestStop(); };
    Watchdog guard(*rig.host);
    TESSERA_CHECK(rig.host->Run() == 0);
    TESSERA_CHECK(!guard.Fired());
    TESSERA_CHECK(rig.host->TickCount() == 1);
    // The tick went on to its network phase and handed the datagrams over (one flush), and the shutdown
    // flushed once more before OnShutdown.
    const std::vector<std::string> expected = {"start", "tick", "flush", "flush", "shutdown"};
    TESSERA_CHECK(rig.logic.events == expected);
}

TESSERA_TEST(host_stop_inside_a_player_callback) {
    Rig rig;
    ClientReplica client(*rig.network.AddClient(), TestSchema());
    // The game's own tick drives the simulated network, so that Run can be used on this thread.
    rig.logic.on_tick_hook = [&] {
        rig.network.Advance(++rig.now);
        client.Poll();
    };
    rig.logic.on_connected_hook = [&](ConnectionId) { rig.host->RequestStop(); };
    Watchdog guard(*rig.host);
    TESSERA_CHECK(rig.host->Run() == 0);
    TESSERA_CHECK(!guard.Fired());
    const auto& events = rig.logic.events;
    TESSERA_CHECK(Count(events, "connected:0") == 1);
    TESSERA_CHECK(Count(events, "shutdown") == 1);
    // The tick that accepted the player finished: nothing ticked after the callback, and its network phase
    // flushed before the shutdown did.
    const size_t connected = IndexOf(events, "connected:0");
    TESSERA_CHECK(Count(std::vector<std::string>(events.begin() + static_cast<std::ptrdiff_t>(connected),
                                                 events.end()),
                        "tick") == 0);
    TESSERA_CHECK(Count(std::vector<std::string>(events.begin() + static_cast<std::ptrdiff_t>(connected),
                                                 events.end()),
                        "flush") == 2);
}

TESSERA_TEST(host_stop_from_another_thread) {
    Rig rig;
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        rig.host->RequestStop();
    });
    const auto start = std::chrono::steady_clock::now();
    Watchdog guard(*rig.host);
    const int code = rig.host->Run();
    TESSERA_CHECK(!guard.Fired());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    stopper.join();
    TESSERA_CHECK(code == 0);
    TESSERA_CHECK(elapsed < std::chrono::seconds(2));
    TESSERA_CHECK(rig.logic.ticks >= 1);
    TESSERA_CHECK(Count(rig.logic.events, "shutdown") == 1);
}

TESSERA_TEST(host_request_stop_inside_on_shutdown_is_harmless) {
    Rig rig;
    rig.logic.on_tick_hook = [&] { rig.host->RequestStop(); };
    rig.logic.on_shutdown_hook = [&] {
        rig.host->RequestStop();
        rig.host->Shutdown(); // not allowed to start the shutdown over
    };
    Watchdog guard(*rig.host);
    TESSERA_CHECK(rig.host->Run() == 0);
    TESSERA_CHECK(!guard.Fired());
    TESSERA_CHECK(Count(rig.logic.events, "shutdown") == 1);
}

TESSERA_TEST(host_shutdown_flushes_before_on_shutdown) {
    Rig rig;
    rig.host->Step();
    rig.logic.events.clear();
    rig.host->Shutdown();
    const std::vector<std::string> expected = {"flush", "shutdown"};
    TESSERA_CHECK(rig.logic.events == expected);
}

TESSERA_TEST(host_accepts_no_player_after_shutdown) {
    Rig rig;
    rig.Step();
    rig.Step();
    rig.host->Shutdown();
    const uint64_t ticks = rig.host->TickCount();
    ClientReplica late(*rig.network.AddClient(), TestSchema());
    for (int i = 0; i < 6; ++i) {
        rig.network.Advance(++rig.now);
        late.Poll();
        rig.host->Step(); // no longer does anything: nobody reads the transport
    }
    TESSERA_CHECK(Count(rig.logic.events, "connected:0") == 0);
    TESSERA_CHECK(rig.host->Replication().ConnectionCount() == 0);
    TESSERA_CHECK(rig.host->TickCount() == ticks);
    TESSERA_CHECK(late.State() != ClientState::Connected);
}

TESSERA_TEST(host_exception_in_on_tick_is_logged_runs_shutdown_and_returns_1) {
    Rig rig;
    rig.logic.on_tick_hook = [&] {
        if (rig.logic.ticks == 2) {
            throw std::runtime_error("the game broke");
        }
    };
    Watchdog guard(*rig.host);
    const int code = rig.host->Run();
    TESSERA_CHECK(!guard.Fired());
    TESSERA_CHECK(code == 1);
    TESSERA_CHECK(Count(rig.logic.events, "shutdown") == 1);
    TESSERA_CHECK(rig.logic.events.back() == "shutdown");
    bool logged = false;
    for (const std::string& line : rig.lines) {
        logged = logged || line.find("the game broke") != std::string::npos;
    }
    TESSERA_CHECK(logged);
}

TESSERA_TEST(host_shutdown_without_start_does_nothing) {
    Rig rig;
    rig.host->Shutdown();
    TESSERA_CHECK(rig.logic.events.empty());
}

TESSERA_TEST(host_exception_in_the_shutdown_flush_returns_1_and_still_calls_on_shutdown) {
    Rig rig;
    rig.logic.on_tick_hook = [&] {
        if (rig.logic.ticks == 2) {
            rig.host->RequestStop();
        }
    };
    rig.logic.on_flush_hook = [&] {
        const auto& events = rig.logic.events; // a flush right after a flush is the one before OnShutdown
        if (events.size() >= 2 && events[events.size() - 2] == "flush") {
            throw std::runtime_error("the flush broke");
        }
    };
    bool escaped = false;
    int code = -1;
    {
        Watchdog guard(*rig.host);
        try {
            code = rig.host->Run();
        } catch (...) {
            escaped = true;
        }
        TESSERA_CHECK(!guard.Fired());
    }
    TESSERA_CHECK(!escaped);
    TESSERA_CHECK(code == 1);
    TESSERA_CHECK(Count(rig.logic.events, "shutdown") == 1);
    bool logged = false;
    for (const std::string& line : rig.lines) {
        logged = logged || line.find("the flush broke") != std::string::npos;
    }
    TESSERA_CHECK(logged);
}

TESSERA_TEST(host_exception_in_on_shutdown_returns_1) {
    Rig rig;
    rig.logic.on_tick_hook = [&] { rig.host->RequestStop(); };
    rig.logic.on_shutdown_hook = [] { throw std::runtime_error("the shutdown broke"); };
    bool escaped = false;
    int code = -1;
    {
        Watchdog guard(*rig.host);
        try {
            code = rig.host->Run();
        } catch (...) {
            escaped = true;
        }
        TESSERA_CHECK(!guard.Fired());
    }
    TESSERA_CHECK(!escaped);
    TESSERA_CHECK(code == 1);
    bool logged = false;
    for (const std::string& line : rig.lines) {
        logged = logged || line.find("the shutdown broke") != std::string::npos;
    }
    TESSERA_CHECK(logged);
}

TESSERA_TEST(host_failed_on_start_is_remembered_and_nothing_ticks) {
    Rig rig;
    int starts = 0;
    rig.logic.on_start_hook = [&] {
        ++starts;
        throw std::runtime_error("the start broke");
    };
    Watchdog guard(*rig.host);
    TESSERA_CHECK(rig.host->Run() == 1);
    TESSERA_CHECK(!guard.Fired());
    rig.host->Step(); // the host has already failed to start: nothing runs
    rig.host->Step();
    TESSERA_CHECK(rig.logic.ticks == 0);
    TESSERA_CHECK(rig.host->TickCount() == 0);
    TESSERA_CHECK(starts == 1);
    TESSERA_CHECK(rig.host->Run() == 1); // and asking again does not start it, nor run it
    TESSERA_CHECK(rig.logic.ticks == 0);
    TESSERA_CHECK(starts == 1);
}

TESSERA_TEST(host_step_after_a_failed_on_start_does_not_tick) {
    Rig rig;
    rig.logic.on_start_hook = [] { throw std::runtime_error("the start broke"); };
    bool threw = false;
    try {
        rig.host->Step();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    TESSERA_CHECK(threw);
    rig.host->Step();
    TESSERA_CHECK(rig.logic.ticks == 0);
    TESSERA_CHECK(rig.host->TickCount() == 0);
}
