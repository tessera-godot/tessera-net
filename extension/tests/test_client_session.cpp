// SPDX-License-Identifier: Apache-2.0
#include "example_game.hpp"
#include "tessera/ext/client_session.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/protocol.hpp"
#include "tessera/net/transport/simulated_network.hpp"
#include "tessera/server/server_host.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace tessera::ext;
namespace net = tessera::net;
namespace server = tessera::server;
using Kind = SessionEvent::Kind;

namespace {

// A small game: a few entities of type 1 that every player sees, which the test can add, move and remove.
class TestGame final : public server::IGameLogic {
public:
    explicit TestGame(float step = 0.01f, uint32_t entities = 3) : step_(step), count_(entities) {
        schema_.AddType(1, {net::PropertyDescriptor::Position(TESSERA_COLUMN(Columns, x),
                                                              TESSERA_COLUMN(Columns, y),
                                                              TESSERA_COLUMN(Columns, z), step_, 2048.0f)});
    }

    server::WorldSetup DescribeWorld() override {
        server::WorldSetup setup;
        setup.schema = &schema_;
        setup.relevancy.types[1] = net::TypeRelevancy{net::RelevancyMode::AlwaysRelevant, 0.0f, 0.0f};
        setup.replication.rate_kbps = 0;
        setup.replication.max_buffer_bytes = 65535;
        return setup;
    }

    void OnStart(server::ServerContext& context) override {
        context_ = &context;
        table_ = std::make_unique<net::EntityTable<Columns>>(net::TableConfig{context.shard_id, 1});
        for (uint32_t i = 0; i < count_; ++i) {
            ids_.push_back(Add(10.0f + 5.0f * static_cast<float>(i), 20.0f + 7.0f * static_cast<float>(i)));
        }
    }

    void OnTick(double) override {
        context_->replication.Refresh(*table_);
    }

    net::EntityId Add(float x, float z) {
        const net::EntityId id = table_->Create();
        const auto where = table_->Find(id);
        Columns& columns = table_->Columns(where->page);
        columns.x[where->slot] = x;
        columns.y[where->slot] = 0.0f;
        columns.z[where->slot] = z;
        table_->MarkDirty(id);
        context_->grid.Insert(id, x, 0.0f, z);
        context_->replication.Relevancy().OnEntityCreated(id);
        return id;
    }

    void Remove(net::EntityId id) {
        context_->grid.Remove(id);
        table_->Destroy(id);
        context_->replication.Relevancy().OnEntityDestroyed(id);
        ids_.erase(std::remove(ids_.begin(), ids_.end(), id), ids_.end());
    }

    void Move(net::EntityId id, float x, float z) {
        const auto where = table_->Find(id);
        Columns& columns = table_->Columns(where->page);
        columns.x[where->slot] = x;
        columns.z[where->slot] = z;
        table_->MarkDirty(id);
    }

    const std::vector<net::EntityId>& Ids() const {
        return ids_;
    }

private:
    struct Columns {
        float x[net::kPageSlots];
        float y[net::kPageSlots];
        float z[net::kPageSlots];
    };

    float step_;
    uint32_t count_;
    net::ReplicationSchema schema_;
    server::ServerContext* context_ = nullptr;
    std::unique_ptr<net::EntityTable<Columns>> table_;
    std::vector<net::EntityId> ids_;
};

// A server over a simulated network. Nothing runs by itself: the test moves time.
struct World {
    explicit World(server::IGameLogic& game, uint32_t shard = 0, uint32_t max_players = 8)
        : network(net::NetworkConditions{}), log(server::LogLevel::Error, [](const std::string&) {}),
          host(MakeConfig(shard, max_players), game, network.Server(), log) {
    }

    static server::ServerConfig MakeConfig(uint32_t shard, uint32_t max_players) {
        server::ServerConfig config;
        config.shard_id = shard;
        config.max_players = max_players;
        config.tick_rate_hz = 100;
        return config;
    }

    // One tick: the network moves, the session reads, the server steps.
    void Step(ClientSession* session = nullptr, std::vector<SessionEvent>* events = nullptr) {
        network.Advance(++now);
        if (session != nullptr) {
            std::vector<SessionEvent> out;
            session->Poll(out);
            if (events != nullptr) {
                events->insert(events->end(), out.begin(), out.end());
            }
        }
        host.Step();
    }

    template <class Done> bool StepUntil(ClientSession& session, std::vector<SessionEvent>& events, Done done) {
        for (int i = 0; i < 200; ++i) {
            Step(&session, &events);
            if (done()) {
                return true;
            }
        }
        return false;
    }

    // Steps until the session is connected and holds `entities` entities.
    bool Converge(ClientSession& session, std::vector<SessionEvent>& events, size_t entities) {
        return StepUntil(session, events, [&] {
            return session.State() == SessionState::Connected && session.Entities().size() == entities;
        });
    }

    // A transport that hands the session one more client of the simulated network.
    TransportFactory Factory() {
        return [this](const std::string&, uint16_t) {
            return std::unique_ptr<net::ITransport>(new Borrowed(*network.AddClient()));
        };
    }

    // The end of a connection that the simulated network owns.
    class Borrowed : public net::ITransport {
    public:
        explicit Borrowed(net::ITransport& inner) : inner_(inner) {
        }
        void Poll(std::vector<net::TransportEvent>& out) override {
            if (hook) {
                hook();
            }
            inner_.Poll(out);
        }
        bool Send(net::TransportConnection c, net::Channel ch, std::span<const uint8_t> data) override {
            return inner_.Send(c, ch, data);
        }
        void Disconnect(net::TransportConnection c) override {
            inner_.Disconnect(c);
        }
        net::TransportStats Stats() const noexcept override {
            return inner_.Stats();
        }
        std::function<void()> hook;

    private:
        net::ITransport& inner_;
    };

    net::SimulatedNetwork network;
    server::Logger log;
    server::ServerHost host;
    uint64_t now = 0;
};

std::shared_ptr<const BuiltSchema> ClientSchema(double step = 0.01) {
    PropertyDescription position;
    position.name = "position";
    position.kind = "position";
    position.step = step;
    position.range = 2048.0;
    const TypeDescription type{1, {position}};
    BuildResult result = BuildSchema(std::span<const TypeDescription>(&type, 1));
    return std::shared_ptr<const BuiltSchema>(std::move(result.schema));
}

size_t Count(const std::vector<SessionEvent>& events, Kind kind) {
    return static_cast<size_t>(std::count_if(events.begin(), events.end(),
                                             [kind](const SessionEvent& e) { return e.kind == kind; }));
}

std::vector<Kind> Kinds(const std::vector<SessionEvent>& events) {
    std::vector<Kind> kinds;
    for (const SessionEvent& e : events) {
        kinds.push_back(e.kind);
    }
    return kinds;
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

bool Near(double a, double b, double tolerance = 0.0100001) {
    return std::fabs(a - b) <= tolerance;
}

// A factory for tests that never connect.
TransportFactory NoTransport() {
    return [](const std::string&, uint16_t) -> std::unique_ptr<net::ITransport> {
        throw std::runtime_error("no transport in this test");
    };
}

} // namespace

TESSERA_TEST(session_starts_disconnected_and_everything_is_empty) {
    ClientSession session(NoTransport());
    TESSERA_CHECK(session.State() == SessionState::Disconnected);
    TESSERA_CHECK(session.Generation() == 0);
    TESSERA_CHECK(session.Entities().empty());
    TESSERA_CHECK(session.Snapshot().empty());
    TESSERA_CHECK(!session.Property(1, "position").has_value());
    TESSERA_CHECK(session.RejectionText().empty());
}

TESSERA_TEST(session_poll_get_and_disconnect_before_connect_are_harmless) {
    ClientSession session(NoTransport());
    for (int i = 0; i < 3; ++i) {
        std::vector<SessionEvent> events;
        session.Poll(events);
        session.Disconnect();
        TESSERA_CHECK(events.empty());
        TESSERA_CHECK(session.State() == SessionState::Disconnected);
        TESSERA_CHECK(session.Entities().empty());
        TESSERA_CHECK(session.Snapshot().empty());
        TESSERA_CHECK(!session.Property(0, "position").has_value());
        TESSERA_CHECK(session.RejectionText().empty());
    }
    TESSERA_CHECK(session.Generation() == 0);
}

TESSERA_TEST(session_connect_rejects_null_schema_bad_port_and_throwing_factory) {
    ClientSession session(NoTransport());
    std::string error;
    TESSERA_CHECK(!session.Connect(nullptr, "127.0.0.1", 7777, error));
    TESSERA_CHECK(Contains(error, "schema"));
    for (int port : {0, -1, 65536, 70000}) {
        error.clear();
        TESSERA_CHECK(!session.Connect(ClientSchema(), "127.0.0.1", port, error));
        TESSERA_CHECK(Contains(error, "port"));
        TESSERA_CHECK(session.State() == SessionState::Disconnected);
    }
    error.clear();
    TESSERA_CHECK(!session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    TESSERA_CHECK(Contains(error, "could not connect: no transport in this test"));
    TESSERA_CHECK(session.State() == SessionState::Disconnected);
    TESSERA_CHECK(session.Generation() == 0);
}

TESSERA_TEST(session_connects_and_reports_the_state_changes) {
    TestGame game;
    World world(game);
    ClientSession session(world.Factory());
    std::string error;
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    TESSERA_CHECK(error.empty());
    TESSERA_CHECK(session.State() == SessionState::Connecting);
    TESSERA_CHECK(session.Generation() == 1);
    std::vector<SessionEvent> events;
    world.Step(&session, &events);
    TESSERA_CHECK(session.State() == SessionState::Handshaking);
    TESSERA_CHECK(world.StepUntil(session, events, [&] { return session.State() == SessionState::Connected; }));
    TESSERA_CHECK(Count(events, Kind::Connected) == 1);
    TESSERA_CHECK(Count(events, Kind::Rejected) == 0 && Count(events, Kind::Disconnected) == 0);
    // More reading does not say it again.
    for (int i = 0; i < 5; ++i) {
        world.Step(&session, &events);
    }
    TESSERA_CHECK(Count(events, Kind::Connected) == 1);
}

TESSERA_TEST(session_entities_appear_with_state_and_match_the_server) {
    server::example::ExampleGame game(100, false);
    World world(game);
    ClientSession session(world.Factory());
    std::string error;
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    std::vector<SessionEvent> events;
    TESSERA_CHECK(world.Converge(session, events, 100));
    TESSERA_CHECK(Count(events, Kind::EntityAppeared) == 100);
    TESSERA_CHECK(Count(events, Kind::EntityRemoved) == 0);
    for (uint64_t id : session.Entities()) {
        const auto position = session.Property(id, "position");
        TESSERA_CHECK(position.has_value());
        if (!position) {
            continue;
        }
        TESSERA_CHECK(position->kind == Value::Kind::Vector3);
        const auto [x, z] = server::example::ExampleGame::InitialPosition(net::EntityId::FromRaw(id).index());
        TESSERA_CHECK(Near(position->vec[0], x));
        TESSERA_CHECK(Near(position->vec[1], 0.0));
        TESSERA_CHECK(Near(position->vec[2], z));
    }
}

TESSERA_TEST(session_entity_removed_arrives_when_the_server_destroys_it) {
    TestGame game(0.01f, 3);
    World world(game);
    ClientSession session(world.Factory());
    std::string error;
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    std::vector<SessionEvent> events;
    TESSERA_CHECK(world.Converge(session, events, 3));
    const net::EntityId doomed = game.Ids()[1];
    game.Remove(doomed);
    events.clear();
    TESSERA_CHECK(world.StepUntil(session, events, [&] { return session.Entities().size() == 2; }));
    TESSERA_CHECK(Count(events, Kind::EntityRemoved) == 1);
    TESSERA_CHECK(Count(events, Kind::EntityAppeared) == 0);
    for (const SessionEvent& e : events) {
        if (e.kind == Kind::EntityRemoved) {
            TESSERA_CHECK(e.entity == doomed.raw());
        }
    }
    TESSERA_CHECK(!session.Property(doomed.raw(), "position").has_value());
}

TESSERA_TEST(session_snapshot_has_every_entity_and_its_named_values) {
    TestGame game(0.01f, 3);
    World world(game);
    ClientSession session(world.Factory());
    std::string error;
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    std::vector<SessionEvent> events;
    TESSERA_CHECK(world.Converge(session, events, 3));
    const std::vector<EntityView> snapshot = session.Snapshot();
    TESSERA_CHECK(snapshot.size() == 3);
    for (size_t i = 0; i < snapshot.size() && i < 3; ++i) {
        TESSERA_CHECK(snapshot[i].type == 1);
        TESSERA_CHECK(snapshot[i].id == session.Entities().at(i));
        TESSERA_CHECK(snapshot[i].properties.size() == 1);
        if (snapshot[i].properties.size() == 1) {
            TESSERA_CHECK(snapshot[i].properties[0].name == "position");
            const Value& v = snapshot[i].properties[0].value;
            const double index = static_cast<double>(net::EntityId::FromRaw(snapshot[i].id).index());
            TESSERA_CHECK(Near(v.vec[0], 10.0 + 5.0 * index));
            TESSERA_CHECK(Near(v.vec[2], 20.0 + 7.0 * index));
        }
    }
}

TESSERA_TEST(session_unknown_entity_and_unknown_name_give_nothing) {
    TestGame game(0.01f, 3);
    World world(game, 5);
    ClientSession session(world.Factory());
    std::string error;
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    std::vector<SessionEvent> events;
    TESSERA_CHECK(world.Converge(session, events, 3));
    const net::EntityId real = game.Ids()[0];
    TESSERA_CHECK(session.Property(real.raw(), "position").has_value());
    TESSERA_CHECK(!session.Property(real.raw(), "").has_value());
    TESSERA_CHECK(!session.Property(real.raw(), "Position").has_value());
    TESSERA_CHECK(!session.Property(real.raw(), "positio").has_value());
    // An id that was never sent, and one of the right shape from a shard this server is not.
    TESSERA_CHECK(!session.Property(net::EntityId::Make(5, 1, 99, 0).raw(), "position").has_value());
    TESSERA_CHECK(!session.Property(net::EntityId::Make(6, 1, real.index(), real.generation()).raw(), "position")
                       .has_value());
    TESSERA_CHECK(!session.Property(0, "position").has_value());
}

TESSERA_TEST(session_schema_mismatch_is_rejected_with_a_clear_text) {
    TestGame game(0.02f, 3); // the server counts in steps of 2 cm, the client in steps of 1 cm
    World world(game);
    ClientSession session(world.Factory());
    std::string error;
    TESSERA_CHECK(session.Connect(ClientSchema(0.01), "127.0.0.1", 7777, error));
    std::vector<SessionEvent> events;
    TESSERA_CHECK(world.StepUntil(session, events, [&] { return session.State() == SessionState::Rejected; }));
    TESSERA_CHECK(Count(events, Kind::Rejected) == 1);
    TESSERA_CHECK(Count(events, Kind::Connected) == 0);
    std::string reason;
    for (const SessionEvent& e : events) {
        if (e.kind == Kind::Rejected) {
            reason = e.reason;
        }
    }
    for (const char* word : {"schema", "types", "kind", "order", "step", "range", "bits", "min", "max", "signed", "count"}) {
        TESSERA_CHECK(Contains(reason, word));
    }
    TESSERA_CHECK(session.RejectionText() == reason);
    TESSERA_CHECK(session.Entities().empty());
    // Staying rejected does not repeat the event.
    for (int i = 0; i < 5; ++i) {
        world.Step(&session, &events);
    }
    TESSERA_CHECK(Count(events, Kind::Rejected) == 1);
    TESSERA_CHECK(session.State() == SessionState::Rejected);
}

TESSERA_TEST(session_version_or_full_server_is_rejected_with_its_own_text) {
    // A server with room for one player: the second one is told it is full.
    TestGame game(0.01f, 3);
    World world(game, 0, 1);
    ClientSession first(world.Factory());
    ClientSession second(world.Factory());
    std::string error;
    TESSERA_CHECK(first.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    std::vector<SessionEvent> first_events;
    TESSERA_CHECK(world.Converge(first, first_events, 3));
    TESSERA_CHECK(second.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    std::vector<SessionEvent> second_events;
    TESSERA_CHECK(world.StepUntil(second, second_events,
                                  [&] { return second.State() == SessionState::Rejected; }));
    TESSERA_CHECK(Contains(second.RejectionText(), "full"));

    // A server that speaks another protocol version: played by the test, since a real one is not at hand.
    net::SimulatedNetwork network(net::NetworkConditions{});
    ClientSession other([&network](const std::string&, uint16_t) {
        return std::unique_ptr<net::ITransport>(new World::Borrowed(*network.AddClient()));
    });
    TESSERA_CHECK(other.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    std::vector<SessionEvent> events;
    for (int tick = 1; tick < 30 && other.State() != SessionState::Rejected; ++tick) {
        network.Advance(static_cast<uint64_t>(tick));
        std::vector<net::TransportEvent> inbox;
        network.Server().Poll(inbox);
        for (const net::TransportEvent& event : inbox) {
            if (event.kind == net::TransportEvent::Kind::Message) {
                std::vector<uint8_t> reply;
                net::Encode(net::ServerHello{false, net::RejectReason::VersionMismatch, 99, 0, 30, 16, 32}, reply);
                network.Server().Send(event.connection, net::Channel::ReliableOrdered, reply);
            }
        }
        network.Advance(static_cast<uint64_t>(tick) + 100);
        other.Poll(events);
    }
    TESSERA_CHECK(other.State() == SessionState::Rejected);
    TESSERA_CHECK(Contains(other.RejectionText(), "version"));
    TESSERA_CHECK(other.RejectionText() != second.RejectionText());
}

TESSERA_TEST(session_server_gone_gives_removed_then_disconnected) {
    TestGame game(0.01f, 5);
    World world(game);
    ClientSession session(world.Factory());
    std::string error;
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    std::vector<SessionEvent> events;
    TESSERA_CHECK(world.Converge(session, events, 5));
    events.clear();
    world.network.Server().Disconnect(1); // the server numbers its connections from 1
    TESSERA_CHECK(world.StepUntil(session, events, [&] { return session.State() == SessionState::Disconnected; }));
    TESSERA_CHECK(events.size() == 6);
    if (events.size() == 6) {
        for (size_t i = 0; i < 5; ++i) {
            TESSERA_CHECK(events[i].kind == Kind::EntityRemoved);
        }
        TESSERA_CHECK(events[5].kind == Kind::Disconnected);
    }
    TESSERA_CHECK(session.Entities().empty());
    TESSERA_CHECK(session.Snapshot().empty());
    for (int i = 0; i < 3; ++i) {
        world.Step(&session, &events);
    }
    TESSERA_CHECK(events.size() == 6);
}

TESSERA_TEST(session_disconnect_is_silent_and_clears_state) {
    TestGame game(0.01f, 3);
    World world(game);
    ClientSession session(world.Factory());
    std::string error;
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    std::vector<SessionEvent> events;
    TESSERA_CHECK(world.Converge(session, events, 3));
    TESSERA_CHECK(world.host.Replication().ConnectionCount() == 1);
    const std::vector<uint64_t> entities_before = session.Entities();
    const uint64_t some_entity = entities_before.empty() ? 0 : entities_before[0];
    session.Disconnect();
    TESSERA_CHECK(session.State() == SessionState::Disconnected);
    TESSERA_CHECK(session.Entities().empty());
    TESSERA_CHECK(!session.Property(some_entity, "position").has_value());
    events.clear();
    for (int i = 0; i < 6; ++i) {
        world.Step(&session, &events);
    }
    TESSERA_CHECK(events.empty());
    TESSERA_CHECK(world.host.Replication().ConnectionCount() == 0); // the server saw the player leave
}

TESSERA_TEST(session_disconnect_twice_and_poll_after_disconnect) {
    TestGame game(0.01f, 3);
    World world(game);
    ClientSession session(world.Factory());
    std::string error;
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    std::vector<SessionEvent> events;
    TESSERA_CHECK(world.Converge(session, events, 3));
    events.clear();
    session.Disconnect();
    session.Disconnect();
    session.Disconnect();
    for (int i = 0; i < 3; ++i) {
        world.Step(&session, &events);
        session.Disconnect();
    }
    TESSERA_CHECK(events.empty());
    TESSERA_CHECK(session.State() == SessionState::Disconnected);
}

TESSERA_TEST(session_reconnect_after_disconnect_starts_clean) {
    TestGame game(0.01f, 3);
    World world(game);
    ClientSession session(world.Factory());
    std::string error;
    std::vector<SessionEvent> events;
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    TESSERA_CHECK(world.Converge(session, events, 3));
    session.Disconnect();
    events.clear();
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    TESSERA_CHECK(world.Converge(session, events, 3));
    TESSERA_CHECK(Count(events, Kind::Connected) == 1);
    TESSERA_CHECK(Count(events, Kind::EntityAppeared) == 3);
    TESSERA_CHECK(Count(events, Kind::EntityRemoved) == 0);
    TESSERA_CHECK(session.Generation() == 3); // connect, disconnect, connect
}

TESSERA_TEST(session_connect_while_connected_replaces_the_connection) {
    TestGame game(0.01f, 3);
    World world(game);
    ClientSession session(world.Factory());
    std::string error;
    std::vector<SessionEvent> events;
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    TESSERA_CHECK(world.Converge(session, events, 3));
    events.clear();
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    TESSERA_CHECK(session.State() == SessionState::Connecting);
    TESSERA_CHECK(session.Entities().empty());
    TESSERA_CHECK(world.Converge(session, events, 3));
    // Nothing from the old connection (no removal, no disconnect) shows up; the new one starts from nothing.
    TESSERA_CHECK(Count(events, Kind::Disconnected) == 0);
    TESSERA_CHECK(Count(events, Kind::EntityRemoved) == 0);
    TESSERA_CHECK(Count(events, Kind::Connected) == 1);
    TESSERA_CHECK(Count(events, Kind::EntityAppeared) == 3);
    for (int i = 0; i < 10; ++i) {
        world.Step(&session, &events);
    }
    TESSERA_CHECK(world.host.Replication().ConnectionCount() == 1); // the old player left
}

TESSERA_TEST(session_generation_changes_on_connect_and_disconnect) {
    TestGame game(0.01f, 1);
    World world(game);
    ClientSession session(world.Factory());
    std::string error;
    const uint64_t start = session.Generation();
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    const uint64_t connected = session.Generation();
    TESSERA_CHECK(connected != start);
    session.Disconnect();
    const uint64_t disconnected = session.Generation();
    TESSERA_CHECK(disconnected != connected);
    session.Disconnect(); // nothing to close: no change
    TESSERA_CHECK(session.Generation() == disconnected);
}

TESSERA_TEST(session_poll_inside_poll_is_ignored) {
    TestGame game(0.01f, 3);
    World world(game);
    ClientSession* self = nullptr;
    std::vector<SessionEvent> inner;
    size_t inner_calls = 0;
    ClientSession session([&](const std::string&, uint16_t) {
        auto transport = std::make_unique<World::Borrowed>(*world.network.AddClient());
        transport->hook = [&] {
            ++inner_calls;
            self->Poll(inner); // the transport calls back into the session while it is being read
        };
        return std::unique_ptr<net::ITransport>(std::move(transport));
    });
    self = &session;
    std::string error;
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    std::vector<SessionEvent> events;
    TESSERA_CHECK(world.Converge(session, events, 3));
    TESSERA_CHECK(inner_calls > 0);
    TESSERA_CHECK(inner.empty());
    TESSERA_CHECK(Count(events, Kind::Connected) == 1);
    TESSERA_CHECK(Count(events, Kind::EntityAppeared) == 3);
}

TESSERA_TEST(session_two_and_three_sessions_are_independent) {
    TestGame game_a(0.01f, 2), game_b(0.01f, 3), game_c(0.01f, 4);
    World world_a(game_a), world_b(game_b), world_c(game_c);
    auto first = std::make_unique<ClientSession>(world_a.Factory());
    auto middle = std::make_unique<ClientSession>(world_b.Factory());
    auto last = std::make_unique<ClientSession>(world_c.Factory());
    std::string error;
    std::vector<SessionEvent> events_a, events_b, events_c;
    TESSERA_CHECK(first->Connect(ClientSchema(), "a", 1, error));
    TESSERA_CHECK(middle->Connect(ClientSchema(), "b", 2, error));
    TESSERA_CHECK(last->Connect(ClientSchema(), "c", 3, error));
    for (int i = 0; i < 40; ++i) {
        world_a.Step(first.get(), &events_a);
        world_b.Step(middle.get(), &events_b);
        world_c.Step(last.get(), &events_c);
    }
    TESSERA_CHECK(first->Entities().size() == 2 && middle->Entities().size() == 3 &&
                  last->Entities().size() == 4);
    // The one in the middle is destroyed while connected and the first one leaves; the last carries on.
    middle.reset();
    first->Disconnect();
    game_c.Move(game_c.Ids()[0], 100.0f, 150.0f);
    for (int i = 0; i < 20; ++i) {
        world_a.Step();
        world_b.Step();
        world_c.Step(last.get(), &events_c);
    }
    const auto moved = last->Property(game_c.Ids()[0].raw(), "position");
    TESSERA_CHECK(moved.has_value());
    if (moved) {
        TESSERA_CHECK(Near(moved->vec[0], 100.0) && Near(moved->vec[2], 150.0));
    }
    TESSERA_CHECK(world_a.host.Replication().ConnectionCount() == 0);
    TESSERA_CHECK(world_b.host.Replication().ConnectionCount() == 0);
    TESSERA_CHECK(world_c.host.Replication().ConnectionCount() == 1);
}

TESSERA_TEST(session_destroyed_while_connected_does_not_touch_the_schema_it_shared) {
    TestGame game(0.01f, 3);
    World world(game);
    std::vector<SessionEvent> events;
    {
        ClientSession session(world.Factory());
        std::string error;
        std::shared_ptr<const BuiltSchema> schema = ClientSchema();
        TESSERA_CHECK(session.Connect(schema, "127.0.0.1", 7777, error));
        schema.reset(); // the caller lets go of its copy; the session still needs the schema
        TESSERA_CHECK(world.Converge(session, events, 3));
        const std::vector<uint64_t> entities = session.Entities();
        TESSERA_CHECK(!entities.empty() && session.Property(entities.empty() ? 0 : entities[0], "position").has_value());
        TESSERA_CHECK(session.Snapshot().size() == 3);
    } // destroyed while connected
    for (int i = 0; i < 5; ++i) {
        world.Step();
    }
    TESSERA_CHECK(world.host.Replication().ConnectionCount() == 0);
}

TESSERA_TEST(session_entity_id_round_trips_through_int64) {
    TestGame game(0.01f, 3);
    World world(game, 4095);
    ClientSession session(world.Factory());
    std::string error;
    std::vector<SessionEvent> events;
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    TESSERA_CHECK(world.Converge(session, events, 3));
    for (uint64_t raw : session.Entities()) {
        TESSERA_CHECK((raw >> 63) == 1); // the shard sets the top bit: a signed number is negative
        const int64_t as_godot_int = static_cast<int64_t>(raw);
        TESSERA_CHECK(as_godot_int < 0);
        const uint64_t back = static_cast<uint64_t>(as_godot_int);
        TESSERA_CHECK(back == raw);
        TESSERA_CHECK(net::EntityId::FromRaw(back).shard() == 4095);
        TESSERA_CHECK(session.Property(back, "position").has_value());
    }
}

TESSERA_TEST(session_events_order_in_one_read) {
    TestGame game(0.01f, 3);
    World world(game);
    ClientSession session(world.Factory());
    std::string error;
    std::vector<SessionEvent> events;
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    TESSERA_CHECK(world.Converge(session, events, 3));
    // The server changes the world, and the client reads nothing until all of it has arrived.
    game.Remove(game.Ids()[0]);
    game.Add(50.0f, 60.0f);
    game.Add(70.0f, 80.0f);
    for (int i = 0; i < 6; ++i) {
        world.network.Advance(++world.now);
        world.host.Step();
    }
    world.network.Advance(++world.now);
    std::vector<SessionEvent> one_read;
    session.Poll(one_read);
    TESSERA_CHECK((Kinds(one_read) == std::vector<Kind>{Kind::EntityRemoved, Kind::EntityAppeared,
                                                        Kind::EntityAppeared}));
}

TESSERA_TEST(session_poll_after_a_protocol_break_ends_cleanly) {
    TestGame game(0.01f, 3);
    World world(game);
    ClientSession session(world.Factory());
    std::string error;
    std::vector<SessionEvent> events;
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    TESSERA_CHECK(world.Converge(session, events, 3));
    events.clear();
    const std::vector<uint8_t> garbage = {0xFF, 0xEE, 0xDD};
    world.network.Server().Send(1, net::Channel::ReliableOrdered, garbage); // connection 1: the first client
    TESSERA_CHECK(world.StepUntil(session, events, [&] { return session.State() == SessionState::Disconnected; }));
    TESSERA_CHECK(events.size() == 4);
    if (events.size() == 4) {
        TESSERA_CHECK(events[0].kind == Kind::EntityRemoved && events[1].kind == Kind::EntityRemoved &&
                      events[2].kind == Kind::EntityRemoved);
        TESSERA_CHECK(events[3].kind == Kind::Disconnected);
    }
    TESSERA_CHECK(session.Entities().empty());
}

namespace {

// The schema of the example server with `players`: type 1 as before, and type 2 with a position and an owner.
std::shared_ptr<const BuiltSchema> PlayersSchema() {
    PropertyDescription position;
    position.name = "position";
    position.kind = "position";
    position.step = 0.01;
    position.range = 2048.0;
    PropertyDescription owner;
    owner.name = "owner";
    owner.kind = "integer";
    owner.min_value = 0;
    owner.max_value = 65535;
    owner.is_signed = false;
    const TypeDescription types[] = {{1, {position}}, {2, {position, owner}}};
    BuildResult result = BuildSchema(std::span<const TypeDescription>(types, 2));
    return std::shared_ptr<const BuiltSchema>(std::move(result.schema));
}

// The entity of the player that owns the connection, or 0 (an id that cannot exist: the type is in it).
uint64_t OwnEntity(const ClientSession& session) {
    for (const uint64_t id : session.Entities()) {
        const auto owner = session.Property(id, "owner");
        if (owner && owner->integer == session.ConnectionId()) {
            return id;
        }
    }
    return 0;
}

// One tick of a world with two sessions.
void StepBoth(World& world, ClientSession& a, ClientSession& b) {
    world.network.Advance(++world.now);
    std::vector<SessionEvent> events;
    a.Poll(events);
    b.Poll(events);
    world.host.Step();
}

} // namespace

TESSERA_TEST(session_send_input_before_connect_and_after_disconnect_returns_0) {
    const uint8_t bytes[] = {1, 2};
    ClientSession idle(NoTransport());
    TESSERA_CHECK(idle.SendInput(bytes) == 0 && idle.ConnectionId() == -1);

    TestGame game;
    World world(game);
    ClientSession session(world.Factory());
    std::string error;
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    TESSERA_CHECK(session.SendInput(bytes) == 0); // connecting
    std::vector<SessionEvent> events;
    TESSERA_CHECK(world.Converge(session, events, 3));
    TESSERA_CHECK(session.SendInput(bytes) == 1);
    session.Disconnect();
    TESSERA_CHECK(session.SendInput(bytes) == 0 && session.ConnectionId() == -1);

    // A server that refuses the schema.
    TestGame other(0.02f, 3);
    World refusing(other);
    ClientSession rejected(refusing.Factory());
    TESSERA_CHECK(rejected.Connect(ClientSchema(0.01), "127.0.0.1", 7777, error));
    std::vector<SessionEvent> rejected_events;
    TESSERA_CHECK(refusing.StepUntil(rejected, rejected_events,
                                     [&] { return rejected.State() == SessionState::Rejected; }));
    TESSERA_CHECK(rejected.SendInput(bytes) == 0 && rejected.ConnectionId() == -1);
}

TESSERA_TEST(session_connection_id_is_minus_1_until_connected_and_then_the_servers_id) {
    TestGame game;
    World world(game);
    ClientSession first(world.Factory());
    ClientSession second(world.Factory());
    std::string error;
    TESSERA_CHECK(first.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    TESSERA_CHECK(first.ConnectionId() == -1); // connecting
    std::vector<SessionEvent> events;
    world.Step(&first, &events);
    TESSERA_CHECK(first.State() == SessionState::Handshaking && first.ConnectionId() == -1);
    TESSERA_CHECK(world.Converge(first, events, 3));
    TESSERA_CHECK(first.ConnectionId() == 0);
    TESSERA_CHECK(second.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    std::vector<SessionEvent> second_events;
    TESSERA_CHECK(world.Converge(second, second_events, 3));
    TESSERA_CHECK(second.ConnectionId() == 1 && first.ConnectionId() == 0);
}

TESSERA_TEST(session_send_input_moves_the_player_entity_of_the_example) {
    server::example::ExampleGame game(0, false, true);
    World world(game);
    ClientSession session(world.Factory());
    std::string error;
    TESSERA_CHECK(session.Connect(PlayersSchema(), "127.0.0.1", 7777, error));
    std::vector<SessionEvent> events;
    TESSERA_CHECK(world.Converge(session, events, 1));
    const uint64_t own = OwnEntity(session);
    TESSERA_CHECK(own != 0);
    const auto start = session.Property(own, "position");
    TESSERA_CHECK(start.has_value());
    const uint8_t east[] = {127, 0};
    bool in_order = true;
    for (uint32_t i = 1; i <= 30; ++i) {
        in_order = in_order && session.SendInput(east) == i;
        world.Step(&session, &events);
    }
    TESSERA_CHECK(in_order);
    for (int i = 0; i < 3; ++i) {
        world.Step(&session, &events);
    }
    const auto end = session.Property(own, "position");
    TESSERA_CHECK(end.has_value());
    if (start && end) {
        TESSERA_CHECK(end->vec[0] > start->vec[0] + 1.0); // 30 ticks of 10 ms at 8 m/s: about 2.4 m
        TESSERA_CHECK(Near(end->vec[2], start->vec[2]));
    }
}

TESSERA_TEST(session_two_sessions_have_independent_sequences_and_players) {
    server::example::ExampleGame game(0, false, true);
    World world(game);
    ClientSession a(world.Factory());
    ClientSession b(world.Factory());
    std::string error;
    TESSERA_CHECK(a.Connect(PlayersSchema(), "127.0.0.1", 7777, error));
    TESSERA_CHECK(b.Connect(PlayersSchema(), "127.0.0.1", 7777, error));
    for (int i = 0; i < 40; ++i) {
        StepBoth(world, a, b);
    }
    TESSERA_CHECK(a.Entities().size() == 2 && b.Entities().size() == 2);
    const uint64_t a_entity = OwnEntity(a);
    const uint64_t b_entity = OwnEntity(b);
    TESSERA_CHECK(a_entity != 0 && b_entity != 0 && a_entity != b_entity);
    const auto a_start = a.Property(a_entity, "position");
    const auto b_start = b.Property(b_entity, "position");
    const uint8_t east[] = {127, 0};
    const uint8_t south[] = {0, 127};
    bool sequences = true;
    for (uint32_t i = 1; i <= 30; ++i) {
        sequences = sequences && a.SendInput(east) == i && b.SendInput(south) == i;
        StepBoth(world, a, b);
    }
    TESSERA_CHECK(sequences);
    for (int i = 0; i < 3; ++i) {
        StepBoth(world, a, b);
    }
    const auto a_end = a.Property(a_entity, "position");
    const auto b_end = b.Property(b_entity, "position");
    TESSERA_CHECK(a_start && b_start && a_end && b_end);
    if (a_start && b_start && a_end && b_end) {
        TESSERA_CHECK(a_end->vec[0] > a_start->vec[0] + 1.0 && Near(a_end->vec[2], a_start->vec[2]));
        TESSERA_CHECK(b_end->vec[2] > b_start->vec[2] + 1.0 && Near(b_end->vec[0], b_start->vec[0]));
    }
}

TESSERA_TEST(session_send_input_with_too_many_bytes_returns_0) {
    TestGame game;
    World world(game);
    ClientSession session(world.Factory());
    std::string error;
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    std::vector<SessionEvent> events;
    TESSERA_CHECK(world.Converge(session, events, 3));
    const std::vector<uint8_t> too_many(net::kMaxInputBytes + 1, 1);
    const std::vector<uint8_t> largest(net::kMaxInputBytes, 1);
    TESSERA_CHECK(session.SendInput(too_many) == 0);
    TESSERA_CHECK(session.SendInput(largest) == 1);
    TESSERA_CHECK(session.SendInput(std::span<const uint8_t>()) == 2);
}

TESSERA_TEST(session_send_input_from_inside_the_poll_hook_works) {
    TestGame game;
    World world(game);
    ClientSession* self = nullptr;
    std::vector<uint32_t> sequences;
    ClientSession session([&](const std::string&, uint16_t) {
        auto transport = std::make_unique<World::Borrowed>(*world.network.AddClient());
        transport->hook = [&] {
            const uint8_t bytes[] = {7};
            sequences.push_back(self->SendInput(bytes)); // from inside `Poll`, as a signal handler would
        };
        return std::unique_ptr<net::ITransport>(std::move(transport));
    });
    self = &session;
    std::string error;
    TESSERA_CHECK(session.Connect(ClientSchema(), "127.0.0.1", 7777, error));
    std::vector<SessionEvent> events;
    TESSERA_CHECK(world.Converge(session, events, 3));
    for (int i = 0; i < 5; ++i) {
        world.Step(&session, &events);
    }
    TESSERA_CHECK(!sequences.empty() && sequences.front() == 0); // not connected yet: nothing sent
    TESSERA_CHECK(sequences.back() != 0);                        // and once it is, the sequence comes back
    TESSERA_CHECK(session.State() == SessionState::Connected);
}

namespace {

// The movement rule of the example server over named values: the same arithmetic as the server's, for the
// "position" property. `scale` makes a rule that does not agree with the server's.
ValueRule PlayerRule(double scale = 1.0) {
    return [scale](std::vector<NamedValue>& state, std::span<const uint8_t> input, double dt) {
        if (input.size() < 2) {
            return true;
        }
        if (input[0] == 99) {
            return false; // a rule that gives up, for the tests that need one
        }
        double dx = std::max<int>(static_cast<int8_t>(input[0]), -127) / 127.0;
        double dz = std::max<int>(static_cast<int8_t>(input[1]), -127) / 127.0;
        const double length = std::sqrt(dx * dx + dz * dz);
        if (length == 0.0) {
            return true;
        }
        if (length > 1.0) {
            dx /= length;
            dz /= length;
        }
        const double speed = server::example::ExampleGame::kPlayerSpeed * scale;
        for (NamedValue& named : state) {
            if (named.name == "position") {
                named.value.vec[0] = std::clamp(named.value.vec[0] + dx * speed * dt, 0.0, 200.0);
                named.value.vec[2] = std::clamp(named.value.vec[2] + dz * speed * dt, 0.0, 200.0);
            }
        }
        return true;
    };
}

// A player of the example server: the world, a connected session and its own entity, with the prediction
// started (zeros are sent until the first state after it arrives).
struct Predicting {
    server::example::ExampleGame game{0, false, true};
    World world{game};
    ClientSession session{world.Factory()};
    std::vector<SessionEvent> events;
    uint64_t own = 0;
    bool ok = false;

    explicit Predicting(ValueRule rule, PredictionSettings settings = {}, bool start = true) {
        std::string error;
        if (!session.Connect(PlayersSchema(), "127.0.0.1", 7777, error) ||
            !world.Converge(session, events, 1)) {
            return;
        }
        own = OwnEntity(session);
        if (!start) {
            ok = own != 0;
            return;
        }
        ok = own != 0 && session.SetPrediction(own, {"position"}, std::move(rule), settings, error);
        for (int i = 0; ok && i < 60 && session.Prediction().status != net::PredictionStatus::Active; ++i) {
            Tick(0, 0);
        }
        ok = ok && session.Prediction().status == net::PredictionStatus::Active;
    }

    void Tick(int8_t x, int8_t z) {
        const uint8_t bytes[2] = {static_cast<uint8_t>(x), static_cast<uint8_t>(z)};
        session.SendInput(bytes);
        world.Step(&session, &events);
    }
    std::optional<Value> Predicted() const {
        return session.PredictedProperty(own, "position");
    }
    std::optional<Value> Authoritative() const {
        return session.Property(own, "position");
    }
};

} // namespace

TESSERA_TEST(session_set_prediction_checks_the_entity_type_and_the_names) {
    Predicting p(PlayerRule(), {}, false);
    TESSERA_CHECK(p.ok);
    std::string error;
    // Not connected: nothing to predict on.
    ClientSession idle(NoTransport());
    TESSERA_CHECK(!idle.SetPrediction(p.own, {"position"}, PlayerRule(), {}, error) && !error.empty());
    TESSERA_CHECK(idle.Prediction().status == net::PredictionStatus::Off);

    error.clear();
    TESSERA_CHECK(!p.session.SetPrediction(p.own, {"nope"}, PlayerRule(), {}, error) &&
                  Contains(error, "nope"));
    error.clear();
    TESSERA_CHECK(!p.session.SetPrediction(0, {"position"}, PlayerRule(), {}, error) && !error.empty());
    error.clear();
    const uint64_t unknown_type = net::EntityId::Make(0, 9, 1, 0).raw();
    TESSERA_CHECK(!p.session.SetPrediction(unknown_type, {"position"}, PlayerRule(), {}, error) &&
                  !error.empty());
    error.clear();
    TESSERA_CHECK(!p.session.SetPrediction(p.own, {}, PlayerRule(), {}, error) && !error.empty());
    error.clear();
    TESSERA_CHECK(!p.session.SetPrediction(p.own, {"position", "position"}, PlayerRule(), {}, error) &&
                  !error.empty());
    error.clear();
    TESSERA_CHECK(!p.session.SetPrediction(p.own, {"position"}, ValueRule(), {}, error) && !error.empty());
    error.clear();
    PredictionSettings bad;
    bad.smoothing_seconds = 6.0;
    TESSERA_CHECK(!p.session.SetPrediction(p.own, {"position"}, PlayerRule(), bad, error) && !error.empty());
    bad = PredictionSettings{};
    bad.snap_distance = 0.0;
    TESSERA_CHECK(!p.session.SetPrediction(p.own, {"position"}, PlayerRule(), bad, error) && !error.empty());
    TESSERA_CHECK(p.session.Prediction().status == net::PredictionStatus::Off);

    TESSERA_CHECK(p.session.SetPrediction(p.own, {"position", "owner"}, PlayerRule(), {}, error));
    TESSERA_CHECK(p.session.Prediction().status == net::PredictionStatus::Waiting);
    // The settings can change while it is set, and only within their ranges.
    PredictionSettings fine;
    fine.smoothing_seconds = 0.5;
    fine.snap_distance = 3.0;
    TESSERA_CHECK(p.session.SetPredictionSettings(fine, error));
    TESSERA_CHECK(!p.session.SetPredictionSettings(bad, error) && !error.empty());
}

TESSERA_TEST(session_predicts_with_a_value_rule_and_returns_named_values) {
    Predicting p(PlayerRule());
    TESSERA_CHECK(p.ok);
    const auto before = p.Predicted();
    TESSERA_CHECK(before.has_value() && before->kind == Value::Kind::Vector3);
    const uint8_t east[] = {127, 0};
    TESSERA_CHECK(p.session.SendInput(east) != 0);
    // No `Poll` between the input and the read.
    const auto after = p.Predicted();
    TESSERA_CHECK(after.has_value());
    if (before && after) {
        const double step = server::example::ExampleGame::kPlayerSpeed / 100.0; // the world runs at 100 Hz
        TESSERA_CHECK(Near(after->vec[0] - before->vec[0], step, 0.001));
        TESSERA_CHECK(Near(after->vec[2], before->vec[2], 0.001));
    }
    // The authoritative value did not move with it.
    const auto server_value = p.Authoritative();
    TESSERA_CHECK(server_value.has_value() && before.has_value() && after.has_value());
    if (server_value && after) {
        TESSERA_CHECK(after->vec[0] > server_value->vec[0] + 0.05);
    }
    // The other property is read the ordinary way.
    const auto owner = p.session.PredictedProperty(p.own, "owner");
    TESSERA_CHECK(owner && owner->kind == Value::Kind::Int && owner->integer == p.session.ConnectionId());
}

TESSERA_TEST(session_predicted_value_before_a_rule_is_the_authoritative_one) {
    Predicting p(PlayerRule(), {}, false);
    TESSERA_CHECK(p.ok);
    const auto plain = p.Authoritative();
    const auto predicted = p.Predicted();
    TESSERA_CHECK(plain && predicted);
    if (plain && predicted) {
        TESSERA_CHECK(plain->vec[0] == predicted->vec[0] && plain->vec[2] == predicted->vec[2]);
    }
    TESSERA_CHECK(!p.session.PredictedProperty(p.own, "nope").has_value());
    TESSERA_CHECK(!p.session.PredictedProperty(12345, "position").has_value());
    // While it is set but has not started, the same.
    std::string error;
    TESSERA_CHECK(p.session.SetPrediction(p.own, {"position"}, PlayerRule(), {}, error));
    TESSERA_CHECK(p.session.Prediction().status == net::PredictionStatus::Waiting);
    const auto waiting = p.Predicted();
    TESSERA_CHECK(waiting && plain && waiting->vec[0] == plain->vec[0]);
}

TESSERA_TEST(session_a_failing_value_rule_disables_prediction_once) {
    for (const int how : {0, 1, 2}) {
        ValueRule base = PlayerRule();
        ValueRule rule = [how, base](std::vector<NamedValue>& state, std::span<const uint8_t> input,
                                     double dt) {
            if (input.size() >= 2 && input[0] == 99) {
                if (how == 1) {
                    throw std::runtime_error("the rule broke");
                }
                if (how == 2) {
                    state[0].value.kind = Value::Kind::Int; // a value of the wrong type
                    return true;
                }
            }
            return base(state, input, dt);
        };
        Predicting p(rule);
        TESSERA_CHECK(p.ok);
        for (int i = 0; i < 5; ++i) {
            p.Tick(99, 0); // the rule fails on these
        }
        const PredictionReport report = p.session.Prediction();
        TESSERA_CHECK(report.status == net::PredictionStatus::Failed);
        TESSERA_CHECK(!report.failure.empty());
        TESSERA_CHECK(report.stats.rule_failures == 1);
        // The value read is the server's again.
        const auto predicted = p.Predicted();
        const auto plain = p.Authoritative();
        TESSERA_CHECK(predicted && plain && predicted->vec[0] == plain->vec[0]);
        TESSERA_CHECK(p.session.State() == SessionState::Connected);
        p.Tick(127, 0); // the game goes on
        TESSERA_CHECK(p.session.Prediction().stats.rule_failures == 1);
    }
}

TESSERA_TEST(session_prediction_follows_the_example_server) {
    Predicting p(PlayerRule());
    TESSERA_CHECK(p.ok);
    const auto start = p.Authoritative();
    double worst = 0.0;
    for (int i = 0; i < 60; ++i) {
        p.Tick(127, 0);
        worst = std::max(worst, p.session.Prediction().stats.last_error);
    }
    TESSERA_CHECK(worst <= 0.0101);
    for (int i = 0; i < 40; ++i) {
        p.Tick(0, 0);
    }
    p.session.Advance(10.0);
    const auto shown = p.Predicted();
    const auto plain = p.Authoritative();
    TESSERA_CHECK(start && shown && plain);
    if (start && shown && plain) {
        TESSERA_CHECK(plain->vec[0] > start->vec[0] + 3.0);
        TESSERA_CHECK(Near(shown->vec[0], plain->vec[0], 0.0101));
        TESSERA_CHECK(Near(shown->vec[2], plain->vec[2], 0.0101));
    }
    TESSERA_CHECK(p.session.Prediction().stats.snaps == 0);
}

TESSERA_TEST(session_two_sessions_predict_independently) {
    server::example::ExampleGame game(0, false, true);
    World world(game);
    ClientSession a(world.Factory());
    ClientSession b(world.Factory());
    std::string error;
    TESSERA_CHECK(a.Connect(PlayersSchema(), "a", 1, error) && b.Connect(PlayersSchema(), "b", 2, error));
    bool both = false;
    for (int i = 0; i < 200 && !both; ++i) {
        StepBoth(world, a, b);
        both = a.Entities().size() == 2 && b.Entities().size() == 2;
    }
    TESSERA_CHECK(both);
    const uint64_t a_entity = OwnEntity(a);
    const uint64_t b_entity = OwnEntity(b);
    TESSERA_CHECK(a_entity != 0 && b_entity != 0 && a_entity != b_entity);
    TESSERA_CHECK(a.SetPrediction(a_entity, {"position"}, PlayerRule(1.0), {}, error));
    TESSERA_CHECK(b.SetPrediction(b_entity, {"position"}, PlayerRule(0.5), {}, error));
    const uint8_t zeros[] = {0, 0};
    for (int i = 0; i < 20; ++i) {
        a.SendInput(zeros);
        b.SendInput(zeros);
        StepBoth(world, a, b);
    }
    TESSERA_CHECK(a.Prediction().status == net::PredictionStatus::Active);
    TESSERA_CHECK(b.Prediction().status == net::PredictionStatus::Active);
    const auto a_before = a.PredictedProperty(a_entity, "position");
    const auto b_before = b.PredictedProperty(b_entity, "position");
    const uint8_t east[] = {127, 0};
    a.SendInput(east);
    b.SendInput(east);
    const auto a_after = a.PredictedProperty(a_entity, "position");
    const auto b_after = b.PredictedProperty(b_entity, "position");
    TESSERA_CHECK(a_before && b_before && a_after && b_after);
    if (a_before && b_before && a_after && b_after) {
        const double step = server::example::ExampleGame::kPlayerSpeed / 100.0;
        TESSERA_CHECK(Near(a_after->vec[0] - a_before->vec[0], step, 0.001));
        TESSERA_CHECK(Near(b_after->vec[0] - b_before->vec[0], step * 0.5, 0.001)); // its own rule
    }
    // One session's entity is plain data for the other.
    const auto other = b.PredictedProperty(a_entity, "position");
    const auto plain = b.Property(a_entity, "position");
    TESSERA_CHECK(other && plain && other->vec[0] == plain->vec[0]);
}

TESSERA_TEST(session_advance_moves_the_slide) {
    PredictionSettings slow;
    slow.smoothing_seconds = 5.0; // a long slide, so that a few ticks of correction are still showing
    Predicting p(PlayerRule(0.5), slow); // a rule that is wrong on purpose: half of the speed
    TESSERA_CHECK(p.ok);
    for (int i = 0; i < 30; ++i) {
        p.Tick(127, 0);
    }
    const auto shown = p.Predicted();
    p.session.Advance(-1.0);
    p.session.Advance(std::nan(""));
    const auto unchanged = p.Predicted();
    TESSERA_CHECK(shown && unchanged && shown->vec[0] == unchanged->vec[0]);
    p.session.Advance(100.0);
    const auto done = p.Predicted();
    TESSERA_CHECK(done.has_value());
    if (shown && done) {
        TESSERA_CHECK(std::fabs(done->vec[0] - shown->vec[0]) > 0.02); // the correction was still showing
    }
    // Once it has slid, the value is the corrected one: the server's state plus what is still pending.
    const auto plain = p.Authoritative();
    TESSERA_CHECK(plain && done && done->vec[0] >= plain->vec[0] - 0.0101);
}

TESSERA_TEST(session_clear_prediction_and_disconnect_discard_it) {
    Predicting p(PlayerRule());
    TESSERA_CHECK(p.ok);
    p.Tick(127, 0);
    const uint8_t east[] = {127, 0};
    p.session.SendInput(east);
    const auto predicted = p.Predicted();
    const auto plain = p.Authoritative();
    TESSERA_CHECK(predicted && plain && predicted->vec[0] > plain->vec[0] + 0.01);
    p.session.ClearPrediction();
    TESSERA_CHECK(p.session.Prediction().status == net::PredictionStatus::Off);
    const auto cleared = p.Predicted();
    const auto plain_again = p.Authoritative();
    TESSERA_CHECK(cleared && plain_again && cleared->vec[0] == plain_again->vec[0]);
    p.session.ClearPrediction(); // harmless twice

    std::string error;
    TESSERA_CHECK(p.session.SetPrediction(p.own, {"position"}, PlayerRule(), {}, error));
    for (int i = 0; i < 20; ++i) {
        p.Tick(0, 0);
    }
    TESSERA_CHECK(p.session.Prediction().status == net::PredictionStatus::Active);
    p.session.Disconnect();
    TESSERA_CHECK(p.session.Prediction().status == net::PredictionStatus::Off);
    TESSERA_CHECK(!p.session.PredictedProperty(p.own, "position").has_value());
    p.session.Advance(1.0); // harmless
}

namespace {

// A rule that calls back into the session it belongs to, which no rule may do. It only does so a few times,
// so that a session that lets it recurse still ends.
struct SessionCallbacks {
    ClientSession* session = nullptr;
    bool armed = false;
    int calls = 0;
    int depth = 0;
    int deepest = 0;
    bool running_inside = false;
    uint32_t sent = 99;
    bool set_prediction = true;
    bool connect = true;
    bool settings = true;
    std::string set_error;
    std::string connect_error;

    ValueRule Rule(ValueRule base) {
        return [this, base](std::vector<NamedValue>& state, std::span<const uint8_t> input, double dt) {
            ++depth;
            deepest = std::max(deepest, depth);
            if (armed && ++calls <= 3) {
                running_inside = session->RuleRunning();
                const uint8_t bytes[2] = {127, 0};
                sent = session->SendInput(bytes);
                std::vector<SessionEvent> out;
                session->Poll(out);
                session->Advance(1.0);
                std::string error;
                settings = session->SetPredictionSettings(PredictionSettings{0.5, 3.0}, error);
                set_prediction =
                    session->SetPrediction(1, {"position"}, ValueRule(base), PredictionSettings{}, set_error);
                connect = session->Connect(PlayersSchema(), "127.0.0.1", 7777, connect_error);
                session->ClearPrediction(); // would destroy the rule that is running
                session->Disconnect();      // would destroy the connection it runs on
            }
            const bool result = base(state, input, dt);
            --depth;
            return result;
        };
    }
};

} // namespace

TESSERA_TEST(session_a_rule_that_calls_back_into_the_session_is_refused) {
    SessionCallbacks back;
    Predicting p(back.Rule(PlayerRule()));
    TESSERA_CHECK(p.ok);
    TESSERA_CHECK(!p.session.RuleRunning());
    back.session = &p.session;
    back.armed = true;
    const uint64_t sent_before = p.session.Prediction().stats.reconciliations;
    p.Tick(127, 0); // sends the input; the rule runs inside, and then inside a reconciliation as well
    TESSERA_CHECK(back.calls >= 1 && back.deepest == 1);
    TESSERA_CHECK(back.running_inside);
    TESSERA_CHECK(back.sent == 0 && !back.set_prediction && !back.connect && !back.settings);
    TESSERA_CHECK(!back.set_error.empty() && !back.connect_error.empty());
    TESSERA_CHECK(p.session.State() == SessionState::Connected);
    TESSERA_CHECK(p.session.Prediction().status == net::PredictionStatus::Active);
    TESSERA_CHECK(p.session.Prediction().stats.reconciliations >= sent_before);
    TESSERA_CHECK(p.session.Prediction().stats.rule_failures == 0);
    // Outside the rule everything works again, the session is the same one.
    back.armed = false;
    TESSERA_CHECK(p.session.SendInput(std::vector<uint8_t>{0, 0}) != 0);
    p.session.ClearPrediction();
    TESSERA_CHECK(p.session.Prediction().status == net::PredictionStatus::Off);
}

TESSERA_TEST(session_the_failure_of_a_rule_is_taken_once) {
    Predicting p(PlayerRule());
    TESSERA_CHECK(p.ok);
    TESSERA_CHECK(p.session.TakeRuleFailure().empty()); // nothing failed
    for (int i = 0; i < 5; ++i) {
        p.Tick(99, 0); // the rule fails on these
    }
    TESSERA_CHECK(p.session.Prediction().status == net::PredictionStatus::Failed);
    const std::string first = p.session.TakeRuleFailure();
    TESSERA_CHECK(!first.empty());
    TESSERA_CHECK(p.session.TakeRuleFailure().empty()); // said once
    p.Tick(99, 0);
    TESSERA_CHECK(p.session.TakeRuleFailure().empty());
    // A new prediction that fails again is a new failure.
    std::string error;
    TESSERA_CHECK(p.session.SetPrediction(p.own, {"position"}, PlayerRule(), {}, error));
    for (int i = 0; i < 60 && p.session.Prediction().status != net::PredictionStatus::Active; ++i) {
        p.Tick(0, 0);
    }
    TESSERA_CHECK(p.session.Prediction().status == net::PredictionStatus::Active);
    for (int i = 0; i < 5; ++i) {
        p.Tick(99, 0);
    }
    TESSERA_CHECK(!p.session.TakeRuleFailure().empty());
    TESSERA_CHECK(p.session.TakeRuleFailure().empty());
}

TESSERA_TEST(session_a_rule_that_leaves_a_number_that_is_not_finite_still_has_a_failure_text) {
    ValueRule base = PlayerRule();
    ValueRule rule = [base](std::vector<NamedValue>& state, std::span<const uint8_t> input, double dt) {
        if (input.size() >= 2 && input[0] == 99) {
            state[0].value.vec[0] = std::nan(""); // right type, no number
            return true;
        }
        return base(state, input, dt);
    };
    Predicting p(rule);
    TESSERA_CHECK(p.ok);
    p.Tick(99, 0);
    TESSERA_CHECK(p.session.Prediction().status == net::PredictionStatus::Failed);
    TESSERA_CHECK(!p.session.TakeRuleFailure().empty());
    TESSERA_CHECK(p.session.TakeRuleFailure().empty());
}

namespace {

// The example server with players, a session that drives (A) and one that watches (B), connected, each with
// the entity of the other and its own. The tick rate of this world is 100 a second.
struct Pair {
    server::example::ExampleGame game{0, false, true};
    World world{game};
    ClientSession a{world.Factory()};
    ClientSession b{world.Factory()};
    uint64_t a_entity = 0;
    uint64_t b_entity = 0;
    bool ok = false;

    Pair() {
        std::string error;
        if (!a.Connect(PlayersSchema(), "a", 1, error) || !b.Connect(PlayersSchema(), "b", 2, error)) {
            return;
        }
        bool both = false;
        for (int i = 0; i < 200 && !both; ++i) {
            StepBoth(world, a, b);
            both = a.Entities().size() == 2 && b.Entities().size() == 2;
        }
        a_entity = OwnEntity(a);
        b_entity = OwnEntity(b);
        ok = both && a_entity != 0 && b_entity != 0 && a_entity != b_entity;
    }

    // One tick: A sends its input, the world steps and both sessions let `seconds` pass.
    void Tick(int8_t x, int8_t z, double seconds = 0.01) {
        const uint8_t bytes[2] = {static_cast<uint8_t>(x), static_cast<uint8_t>(z)};
        a.SendInput(bytes);
        StepBoth(world, a, b);
        a.Advance(seconds);
        b.Advance(seconds);
    }
};

double X(const std::optional<Value>& value) {
    return value ? value->vec[0] : std::nan("");
}

} // namespace

TESSERA_TEST(session_set_interpolation_refuses_bad_values_with_a_reason) {
    ClientSession session(NoTransport());
    std::string error;
    TESSERA_CHECK(!session.SetInterpolation(31, 2.5, error) && Contains(error, "delay_ticks"));
    error.clear();
    TESSERA_CHECK(!session.SetInterpolation(static_cast<uint32_t>(-1), 2.5, error) &&
                  Contains(error, "delay_ticks"));
    for (const double snap : {0.0, -1.0, std::nan(""), std::numeric_limits<double>::infinity()}) {
        error.clear();
        TESSERA_CHECK(!session.SetInterpolation(2, snap, error) && Contains(error, "snap_distance"));
    }
    TESSERA_CHECK(!session.Interpolation().configured); // nothing was set
    TESSERA_CHECK(session.SetInterpolation(0, 0.001, error));
    TESSERA_CHECK(session.SetInterpolation(30, 1000.0, error));
}

TESSERA_TEST(session_interpolation_can_be_set_before_connecting_and_survives_a_reconnect) {
    server::example::ExampleGame game(0, false, true);
    World world(game);
    ClientSession session(world.Factory());
    std::string error;
    TESSERA_CHECK(session.SetInterpolation(4, 3.0, error)); // nothing is connected yet
    InterpolationReport report = session.Interpolation();
    TESSERA_CHECK(report.configured && !report.running && report.delay_ticks == 4 &&
                  report.snap_distance == 3.0);

    std::vector<SessionEvent> events;
    TESSERA_CHECK(session.Connect(PlayersSchema(), "x", 1, error));
    TESSERA_CHECK(world.Converge(session, events, 1));
    report = session.Interpolation();
    TESSERA_CHECK(report.configured && report.running && report.delay_ticks == 4 &&
                  report.snap_distance == 3.0);
    session.Disconnect();
    report = session.Interpolation();
    TESSERA_CHECK(report.configured && !report.running); // the settings stay, the clock does not
    events.clear();
    TESSERA_CHECK(session.Connect(PlayersSchema(), "x", 1, error));
    TESSERA_CHECK(world.Converge(session, events, 1));
    report = session.Interpolation();
    TESSERA_CHECK(report.configured && report.running && report.delay_ticks == 4 &&
                  report.snap_distance == 3.0);

    session.ClearInterpolation();
    TESSERA_CHECK(!session.Interpolation().configured && !session.Interpolation().running);
    session.Disconnect();
    TESSERA_CHECK(session.Connect(PlayersSchema(), "x", 1, error)); // forgotten: it does not come back
    TESSERA_CHECK(!session.Interpolation().configured);
}

TESSERA_TEST(session_interpolated_value_before_set_interpolation_is_the_latest_value) {
    Pair p;
    TESSERA_CHECK(p.ok);
    for (int i = 0; i < 30; ++i) {
        p.Tick(127, 0);
    }
    const auto plain = p.b.Property(p.a_entity, "position");
    const auto interpolated = p.b.InterpolatedProperty(p.a_entity, "position");
    TESSERA_CHECK(plain && interpolated && plain->vec[0] == interpolated->vec[0] &&
                  plain->vec[2] == interpolated->vec[2]);
    const auto owner = p.b.InterpolatedProperty(p.a_entity, "owner");
    TESSERA_CHECK(owner && owner->integer == p.a.ConnectionId());
    // Nothing known stays nothing.
    TESSERA_CHECK(!p.b.InterpolatedProperty(0x123456789ull, "position").has_value());
    TESSERA_CHECK(!p.b.InterpolatedProperty(p.a_entity, "nothing").has_value());
    const InterpolationReport report = p.b.Interpolation();
    TESSERA_CHECK(!report.configured && !report.running && report.stats.states == 0);
}

TESSERA_TEST(session_interpolated_position_lags_the_latest_and_lies_between_two_states) {
    Pair p;
    TESSERA_CHECK(p.ok);
    std::string error;
    TESSERA_CHECK(p.b.SetInterpolation(2, 2.5, error));
    double lowest = 1e9;
    double highest = -1e9;
    double previous = -1e9;
    int compared = 0;
    for (int i = 0; i < 80; ++i) {
        p.Tick(127, 0);
        const double latest = X(p.b.Property(p.a_entity, "position"));
        const double shown = X(p.b.InterpolatedProperty(p.a_entity, "position"));
        lowest = std::min(lowest, latest);
        highest = std::max(highest, latest);
        if (i < 20) {
            continue; // the clock has started and the entity has begun to move
        }
        ++compared;
        TESSERA_CHECK(shown >= lowest - 0.0101 && shown <= highest + 0.0101);
        TESSERA_CHECK(shown >= previous - 0.0101); // never back
        const double behind = latest - shown;
        TESSERA_CHECK(behind > 0.01 && behind <= 0.25); // a couple of ticks of 0.08 m
        previous = shown;
    }
    TESSERA_CHECK(compared == 60);
    const InterpolationReport report = p.b.Interpolation();
    TESSERA_CHECK(report.configured && report.running && report.stats.states > 50);
    TESSERA_CHECK(report.lag_ticks > 0.0 && report.lag_ticks <= 2.0 + 1e-9);
}

namespace {

// One entity that stands still and has a number that the test changes: the held state of an integer
// shows only if it changes.
class HealthGame final : public server::IGameLogic {
public:
    HealthGame() {
        schema_.AddType(1, {net::PropertyDescriptor::Position(TESSERA_COLUMN(Columns, x),
                                                              TESSERA_COLUMN(Columns, y),
                                                              TESSERA_COLUMN(Columns, z), 0.01f, 2048.0f),
                            net::PropertyDescriptor::Integer(TESSERA_COLUMN(Columns, health), false, 0, 65535)});
    }

    server::WorldSetup DescribeWorld() override {
        server::WorldSetup setup;
        setup.schema = &schema_;
        setup.relevancy.types[1] = net::TypeRelevancy{net::RelevancyMode::AlwaysRelevant, 0.0f, 0.0f};
        setup.replication.rate_kbps = 0;
        setup.replication.max_buffer_bytes = 65535;
        return setup;
    }

    void OnStart(server::ServerContext& context) override {
        context_ = &context;
        table_ = std::make_unique<net::EntityTable<Columns>>(net::TableConfig{context.shard_id, 1});
        id_ = table_->Create();
        const auto where = table_->Find(id_);
        Columns& columns = table_->Columns(where->page);
        columns.x[where->slot] = 10.0f;
        columns.y[where->slot] = 0.0f;
        columns.z[where->slot] = 10.0f;
        columns.health[where->slot] = 100;
        table_->MarkDirty(id_);
        context.grid.Insert(id_, 10.0f, 0.0f, 10.0f);
        context.replication.Relevancy().OnEntityCreated(id_);
    }

    void OnTick(double) override {
        context_->replication.Refresh(*table_);
    }

    void SetHealth(uint16_t health) {
        const auto where = table_->Find(id_);
        table_->Columns(where->page).health[where->slot] = health;
        table_->MarkDirty(id_);
    }

    static std::shared_ptr<const BuiltSchema> ClientSide() {
        PropertyDescription position;
        position.name = "position";
        position.kind = "position";
        position.step = 0.01;
        position.range = 2048.0;
        PropertyDescription health;
        health.name = "health";
        health.kind = "integer";
        health.min_value = 0;
        health.max_value = 65535;
        health.is_signed = false;
        const TypeDescription types[] = {{1, {position, health}}};
        BuildResult result = BuildSchema(std::span<const TypeDescription>(types, 1));
        return std::shared_ptr<const BuiltSchema>(std::move(result.schema));
    }

private:
    struct Columns {
        float x[net::kPageSlots];
        float y[net::kPageSlots];
        float z[net::kPageSlots];
        uint16_t health[net::kPageSlots];
    };

    net::ReplicationSchema schema_;
    server::ServerContext* context_ = nullptr;
    std::unique_ptr<net::EntityTable<Columns>> table_;
    net::EntityId id_ = net::EntityId::Null();
};

int64_t Health(const std::optional<Value>& value) {
    return value && value->kind == Value::Kind::Int ? value->integer : -1;
}

} // namespace

TESSERA_TEST(session_interpolated_value_of_a_property_that_is_not_blended_is_the_held_state) {
    HealthGame game;
    World world(game);
    ClientSession session(world.Factory());
    std::string error;
    TESSERA_CHECK(session.SetInterpolation(2, 2.5, error));
    TESSERA_CHECK(session.Connect(HealthGame::ClientSide(), "127.0.0.1", 7777, error));
    std::vector<SessionEvent> events;
    TESSERA_CHECK(world.Converge(session, events, 1));
    TESSERA_CHECK(!session.Entities().empty());
    const uint64_t entity = session.Entities().empty() ? 0 : session.Entities().front();
    for (int i = 0; i < 30; ++i) {
        world.Step(&session, &events);
        session.Advance(0.01);
    }
    TESSERA_CHECK(Health(session.Property(entity, "health")) == 100);
    TESSERA_CHECK(Health(session.InterpolatedProperty(entity, "health")) == 100);
    // The server changes it: what was received is the new value, what is shown is still the old one until the
    // shown time (a little behind the newest) gets to the state that holds it.
    game.SetHealth(40);
    bool lagged = false;
    bool reached = false;
    for (int i = 0; i < 30 && !reached; ++i) {
        world.Step(&session, &events);
        const int64_t received = Health(session.Property(entity, "health"));
        const int64_t shown = Health(session.InterpolatedProperty(entity, "health"));
        TESSERA_CHECK(shown == 100 || shown == 40);
        lagged = lagged || (received == 40 && shown == 100);
        reached = shown == 40;
        session.Advance(0.01);
    }
    TESSERA_CHECK(lagged);
    TESSERA_CHECK(reached);
    TESSERA_CHECK(Health(session.InterpolatedProperty(entity, "health")) == 40);
}

TESSERA_TEST(session_interpolated_value_for_the_predicted_entity_is_the_predicted_one) {
    Pair p;
    TESSERA_CHECK(p.ok);
    std::string error;
    TESSERA_CHECK(p.a.SetInterpolation(2, 2.5, error));
    TESSERA_CHECK(p.a.SetPrediction(p.a_entity, {"position"}, PlayerRule(), {}, error));
    // Right after it is set the prediction waits for a state: the entity is shown as the server last said.
    TESSERA_CHECK(p.a.Prediction().status == net::PredictionStatus::Waiting);
    for (int i = 0; i < 30; ++i) {
        p.Tick(127, 0);
    }
    TESSERA_CHECK(p.a.Prediction().status == net::PredictionStatus::Active);
    const uint8_t east[] = {127, 0};
    p.a.SendInput(east); // the prediction is now ahead of the server
    const auto predicted = p.a.PredictedProperty(p.a_entity, "position");
    const auto interpolated = p.a.InterpolatedProperty(p.a_entity, "position");
    const auto plain = p.a.Property(p.a_entity, "position");
    TESSERA_CHECK(predicted && interpolated && plain);
    if (predicted && interpolated && plain) {
        TESSERA_CHECK(interpolated->vec[0] == predicted->vec[0] && interpolated->vec[2] == predicted->vec[2]);
        TESSERA_CHECK(predicted->vec[0] > plain->vec[0] + 0.01); // and not the server's, nor a lagging copy
    }

    // While the prediction is not active, the server's last word: never the interpolated one.
    for (int i = 0; i < 70; ++i) {
        p.a.SendInput(east); // too many inputs wait for a state: it pauses
    }
    TESSERA_CHECK(p.a.Prediction().status == net::PredictionStatus::Paused);
    {
        const auto shown = p.a.InterpolatedProperty(p.a_entity, "position");
        const auto last = p.a.Property(p.a_entity, "position");
        TESSERA_CHECK(shown && last && shown->vec[0] == last->vec[0]);
    }
    p.a.ClearPrediction();
    TESSERA_CHECK(p.a.SetPrediction(p.a_entity, {"position"}, PlayerRule(), {}, error));
    const uint8_t giving_up[] = {99, 0};
    for (int i = 0; i < 40; ++i) {
        p.a.SendInput(giving_up);
        StepBoth(p.world, p.a, p.b);
        p.a.Advance(0.01);
    }
    TESSERA_CHECK(p.a.Prediction().status == net::PredictionStatus::Failed);
    {
        const auto shown = p.a.InterpolatedProperty(p.a_entity, "position");
        const auto last = p.a.Property(p.a_entity, "position");
        TESSERA_CHECK(shown && last && shown->vec[0] == last->vec[0]);
    }
}

TESSERA_TEST(session_without_prediction_the_own_entity_is_interpolated_like_the_others) {
    Pair p;
    TESSERA_CHECK(p.ok);
    std::string error;
    TESSERA_CHECK(p.a.SetInterpolation(2, 2.5, error));
    TESSERA_CHECK(p.a.Prediction().status == net::PredictionStatus::Off);
    for (int i = 0; i < 60; ++i) {
        p.Tick(127, 0);
    }
    const double latest = X(p.a.Property(p.a_entity, "position"));
    const double shown = X(p.a.InterpolatedProperty(p.a_entity, "position"));
    TESSERA_CHECK(shown < latest - 0.01 && latest - shown <= 0.25);
}

TESSERA_TEST(session_advance_moves_both_the_prediction_and_the_interpolation) {
    Pair p;
    TESSERA_CHECK(p.ok);
    std::string error;
    PredictionSettings slow;
    slow.smoothing_seconds = 5.0;
    // A rule that is wrong on purpose: half of the speed.
    TESSERA_CHECK(p.a.SetPrediction(p.a_entity, {"position"}, PlayerRule(0.5), slow, error));
    TESSERA_CHECK(p.a.SetInterpolation(2, 2.5, error));
    // The other player walks, and this one is predicted with that rule. No time passes for the session (no
    // Advance), so a correction is still to slide and the other entity is behind.
    for (int i = 0; i < 60; ++i) {
        const uint8_t east[] = {127, 0};
        p.a.SendInput(east);
        const uint8_t b_east[] = {127, 0};
        p.b.SendInput(b_east);
        StepBoth(p.world, p.a, p.b);
    }
    TESSERA_CHECK(p.a.Prediction().status == net::PredictionStatus::Active);
    const double before_predicted = X(p.a.PredictedProperty(p.a_entity, "position"));
    const double before_other = X(p.a.InterpolatedProperty(p.b_entity, "position"));
    const double latest_other = X(p.a.Property(p.b_entity, "position"));
    TESSERA_CHECK(before_other < latest_other - 0.05); // behind: the clock has not moved on
    TESSERA_CHECK(p.a.Interpolation().lag_ticks >= 2.0);
    p.a.Advance(0.0);
    p.a.Advance(-1.0);
    TESSERA_CHECK(X(p.a.InterpolatedProperty(p.b_entity, "position")) == before_other); // nothing passed
    p.a.Advance(10.0);
    TESSERA_CHECK(X(p.a.PredictedProperty(p.a_entity, "position")) != before_predicted); // the slide moved
    TESSERA_CHECK(p.a.Interpolation().lag_ticks == 0.0);
    TESSERA_CHECK(X(p.a.InterpolatedProperty(p.b_entity, "position")) ==
                  X(p.a.Property(p.b_entity, "position")));
}

TESSERA_TEST(session_clear_interpolation_and_disconnect_return_the_latest_values) {
    Pair p;
    TESSERA_CHECK(p.ok);
    std::string error;
    TESSERA_CHECK(p.b.SetInterpolation(2, 2.5, error));
    for (int i = 0; i < 40; ++i) {
        p.Tick(127, 0);
    }
    const double lagging = X(p.b.InterpolatedProperty(p.a_entity, "position"));
    const double latest = X(p.b.Property(p.a_entity, "position"));
    TESSERA_CHECK(lagging < latest);
    p.b.ClearInterpolation();
    TESSERA_CHECK(X(p.b.InterpolatedProperty(p.a_entity, "position")) == latest);
    TESSERA_CHECK(!p.b.Interpolation().configured);
    p.b.ClearInterpolation(); // harmless twice
    TESSERA_CHECK(p.b.SetInterpolation(2, 2.5, error));
    p.b.Disconnect();
    TESSERA_CHECK(!p.b.InterpolatedProperty(p.a_entity, "position").has_value());
    TESSERA_CHECK(!p.b.Interpolation().running);
    p.b.Advance(1.0); // harmless
}

TESSERA_TEST(session_two_sessions_interpolate_independently) {
    Pair p;
    TESSERA_CHECK(p.ok);
    std::string error;
    TESSERA_CHECK(p.a.SetInterpolation(2, 2.5, error));
    TESSERA_CHECK(p.b.SetInterpolation(6, 2.5, error));
    for (int i = 0; i < 80; ++i) {
        const uint8_t east[] = {127, 0};
        p.a.SendInput(east);
        p.b.SendInput(east);
        StepBoth(p.world, p.a, p.b);
        p.a.Advance(0.01);
        p.b.Advance(0.01);
    }
    TESSERA_CHECK(p.a.Interpolation().delay_ticks == 2 && p.b.Interpolation().delay_ticks == 6);
    // Each shows the other a different time in the past.
    const double a_behind =
        X(p.a.Property(p.b_entity, "position")) - X(p.a.InterpolatedProperty(p.b_entity, "position"));
    const double b_behind =
        X(p.b.Property(p.a_entity, "position")) - X(p.b.InterpolatedProperty(p.a_entity, "position"));
    TESSERA_CHECK(a_behind > 0.01 && b_behind > a_behind + 0.05);
    TESSERA_CHECK(p.b.Interpolation().lag_ticks > p.a.Interpolation().lag_ticks + 0.5);
    // And one session's settings are not the other's.
    p.a.ClearInterpolation();
    TESSERA_CHECK(!p.a.Interpolation().configured && p.b.Interpolation().configured);
}
