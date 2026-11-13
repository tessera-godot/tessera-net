// SPDX-License-Identifier: Apache-2.0
// The example game with `players` on: one entity for each connected player, moved with the input it sends.
#include "example_game.hpp"
#include "tessera/net/client_replica.hpp"
#include "tessera/net/replication/property_codecs.hpp"
#include "tessera/net/transport/simulated_network.hpp"
#include "tessera/server/server_host.hpp"
#include "test_support.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace tessera::net;
using namespace tessera::server;
using tessera::server::example::ExampleGame;

namespace {

using Position = std::array<double, 3>;

// A client that only reads: it greets the server and keeps the event batches it is sent, so that a test can
// see what the server told it (the replication client hides the kind of an event).
struct Watcher {
    ITransport* transport;
    std::vector<EventBatch> batches;

    void Poll(uint64_t schema_hash) {
        std::vector<TransportEvent> events;
        transport->Poll(events);
        for (const TransportEvent& event : events) {
            if (event.kind == TransportEvent::Kind::Connected) {
                std::vector<uint8_t> hello;
                Encode(ClientHello{kProtocolVersion, schema_hash}, hello);
                transport->Send(0, Channel::ReliableOrdered, hello);
            } else if (event.kind == TransportEvent::Kind::Message) {
                EventBatch batch;
                if (Decode(event.data, &batch)) {
                    batches.push_back(std::move(batch));
                }
            }
        }
    }

    bool SawDestroy(EntityId id) const {
        for (const EventBatch& batch : batches) {
            for (const VisibilityEvent& event : batch.events) {
                if (event.kind == VisibilityEvent::Kind::Destroy && event.id == id) {
                    return true;
                }
            }
        }
        return false;
    }
};

// A host running the example game with players, on a simulated network, and the clients that play.
struct World {
    World()
        : network(NetworkConditions{}), log(LogLevel::Error, [](const std::string&) {}),
          game(0, false, true) {
        config.tick_rate_hz = 30;
        config.max_players = 8;
        schema = game.DescribeWorld().schema;
        host = std::make_unique<ServerHost>(config, game, network.Server(), log);
    }

    Watcher& AddWatcher() {
        watchers.push_back(Watcher{network.AddClient(), {}});
        return watchers.back();
    }

    ClientReplica& AddClient() {
        clients.push_back(std::make_unique<ClientReplica>(*network.AddClient(), *schema));
        return *clients.back();
    }

    // One tick the way a game loop runs it: the network moves, the clients read, the host steps.
    void Step(int count = 1) {
        for (int i = 0; i < count; ++i) {
            network.Advance(++now);
            for (const auto& client : clients) {
                client->Poll();
            }
            for (Watcher& watcher : watchers) {
                watcher.Poll(schema->Hash());
            }
            host->Step();
        }
    }

    // The entity of the player `owner`, as the client sees it (null if it does not).
    EntityId EntityOf(const ClientReplica& client, uint32_t owner) const {
        const TypeSchema* type = schema->Find(ExampleGame::kPlayerType);
        EntityId found = EntityId::Null();
        client.ForEachEntity([&](EntityId id, const uint32_t* words) {
            if (id.type() == ExampleGame::kPlayerType &&
                IntegerValue(type->properties[1], words + type->properties[1].word_offset) == owner) {
                found = id;
            }
        });
        return found;
    }

    Position PositionOf(const ClientReplica& client, EntityId id) const {
        const TypeSchema* type = schema->Find(ExampleGame::kPlayerType);
        const uint32_t* words = client.WordsOf(id);
        Position position{std::nan(""), std::nan(""), std::nan("")};
        if (words != nullptr) {
            DequantizePosition(type->properties[0], words + type->properties[0].word_offset, position.data());
        }
        return position;
    }

    // Connects `client` and waits until it sees its own entity.
    EntityId Join(ClientReplica& client) {
        for (int i = 0; i < 40; ++i) {
            Step();
            if (client.State() == ClientState::Connected &&
                !EntityOf(client, client.ConnectionId()).IsNull()) {
                break;
            }
        }
        Step(3);
        return EntityOf(client, client.ConnectionId());
    }

    // Sends the same input, one byte for each axis, on each of `ticks` ticks.
    void Drive(ClientReplica& client, int8_t axis_x, int8_t axis_z, int ticks) {
        const uint8_t bytes[2] = {static_cast<uint8_t>(axis_x), static_cast<uint8_t>(axis_z)};
        for (int i = 0; i < ticks; ++i) {
            client.SendInput(bytes);
            Step();
        }
    }

    SimulatedNetwork network;
    Logger log;
    ExampleGame game;
    tessera::server::ServerConfig config; // spelled out: `net` has a `ServerConfig` too
    const ReplicationSchema* schema = nullptr;
    std::unique_ptr<ServerHost> host;
    std::vector<std::unique_ptr<ClientReplica>> clients;
    std::vector<Watcher> watchers;
    uint64_t now = 0;
};

double Distance(const Position& a, const Position& b) {
    return std::hypot(a[0] - b[0], a[2] - b[2]);
}

bool Near(double a, double b, double tolerance) {
    return std::abs(a - b) <= tolerance;
}

// A tick moves a player at most this far.
constexpr double kStepPerTick = ExampleGame::kPlayerSpeed / 30.0;

} // namespace

TESSERA_TEST(without_players_the_example_is_what_it_was) {
    ExampleGame game(1, false);
    const ReplicationSchema* schema = game.DescribeWorld().schema;
    TESSERA_CHECK(schema->Find(2) == nullptr);
    TESSERA_CHECK(schema->Hash() == 1615724867110258213ull);
}

TESSERA_TEST(the_players_schema_has_position_and_owner_for_type_2) {
    ExampleGame game(1, false, true);
    const TypeSchema* type = game.DescribeWorld().schema->Find(2);
    TESSERA_CHECK(type != nullptr);
    if (type != nullptr && type->properties.size() == 2) {
        TESSERA_CHECK(type->properties[0].kind == PropertyKind::Position);
        TESSERA_CHECK(type->properties[1].kind == PropertyKind::Integer);
    } else {
        TESSERA_CHECK(false);
    }
}

TESSERA_TEST(a_player_entity_appears_at_the_spawn_with_its_owner) {
    World world;
    ClientReplica& client = world.AddClient();
    const EntityId own = world.Join(client);
    TESSERA_CHECK(!own.IsNull());
    const auto [spawn_x, spawn_z] = ExampleGame::PlayerSpawn(client.ConnectionId());
    const Position position = world.PositionOf(client, own);
    TESSERA_CHECK(Near(position[0], spawn_x, 0.011) && Near(position[2], spawn_z, 0.011));
    TESSERA_CHECK(world.EntityOf(client, client.ConnectionId()) == own);
}

TESSERA_TEST(input_moves_the_player_at_the_speed_limit) {
    World world;
    ClientReplica& client = world.AddClient();
    const EntityId own = world.Join(client);
    const Position start = world.PositionOf(client, own);
    world.Drive(client, 127, 0, 30);
    world.Step(2);
    const Position end = world.PositionOf(client, own);
    const double moved = end[0] - start[0];
    // A second of input at the speed limit, give or take the ticks the input takes to arrive and the state to
    // come back.
    TESSERA_CHECK(moved > ExampleGame::kPlayerSpeed - 4 * kStepPerTick);
    TESSERA_CHECK(moved <= ExampleGame::kPlayerSpeed + 4 * kStepPerTick + 0.02);
    TESSERA_CHECK(Near(end[2], start[2], 0.011));
}

TESSERA_TEST(a_diagonal_input_is_not_faster_than_a_straight_one) {
    World world;
    ClientReplica& straight = world.AddClient();
    ClientReplica& diagonal = world.AddClient();
    const EntityId straight_entity = world.Join(straight);
    const EntityId diagonal_entity = world.Join(diagonal);
    const Position straight_start = world.PositionOf(straight, straight_entity);
    const Position diagonal_start = world.PositionOf(diagonal, diagonal_entity);
    const uint8_t straight_input[2] = {127, 0};
    const uint8_t diagonal_input[2] = {127, 127};
    for (int i = 0; i < 30; ++i) {
        straight.SendInput(straight_input);
        diagonal.SendInput(diagonal_input);
        world.Step();
    }
    world.Step(2);
    const double straight_moved = Distance(straight_start, world.PositionOf(straight, straight_entity));
    const double diagonal_moved = Distance(diagonal_start, world.PositionOf(diagonal, diagonal_entity));
    TESSERA_CHECK(straight_moved > 5.0);
    TESSERA_CHECK(diagonal_moved <= straight_moved + 0.03);
    const Position diagonal_end = world.PositionOf(diagonal, diagonal_entity);
    TESSERA_CHECK(diagonal_end[0] > diagonal_start[0] + 1.0 && diagonal_end[2] > diagonal_start[2] + 1.0);
}

TESSERA_TEST(an_axis_outside_minus_127_to_127_is_clipped) {
    World world;
    ClientReplica& clipped = world.AddClient();
    ClientReplica& limit = world.AddClient();
    const EntityId clipped_entity = world.Join(clipped);
    const EntityId limit_entity = world.Join(limit);
    const Position clipped_start = world.PositionOf(clipped, clipped_entity);
    const Position limit_start = world.PositionOf(limit, limit_entity);
    const uint8_t clipped_input[2] = {0x80, 127}; // -128 on x, which is not a valid axis value
    const uint8_t limit_input[2] = {0x81, 127};   // -127
    for (int i = 0; i < 60; ++i) {
        clipped.SendInput(clipped_input);
        limit.SendInput(limit_input);
        world.Step();
    }
    world.Step(2);
    const Position clipped_end = world.PositionOf(clipped, clipped_entity);
    const Position limit_end = world.PositionOf(limit, limit_entity);
    TESSERA_CHECK(Near(clipped_end[0] - clipped_start[0], limit_end[0] - limit_start[0], 0.02));
    TESSERA_CHECK(Near(clipped_end[2] - clipped_start[2], limit_end[2] - limit_start[2], 0.02));
    TESSERA_CHECK(clipped_end[0] < clipped_start[0] - 1.0);
}

TESSERA_TEST(a_short_input_is_ignored) {
    World world;
    ClientReplica& client = world.AddClient();
    const EntityId own = world.Join(client);
    const Position start = world.PositionOf(client, own);
    const uint8_t one_byte[1] = {127};
    for (int i = 0; i < 10; ++i) {
        client.SendInput(one_byte);
        world.Step();
    }
    world.Step(3);
    const Position end = world.PositionOf(client, own);
    TESSERA_CHECK(Near(end[0], start[0], 0.011) && Near(end[2], start[2], 0.011));
}

TESSERA_TEST(the_player_stops_when_the_input_stops) {
    World world;
    ClientReplica& client = world.AddClient();
    const EntityId own = world.Join(client);
    world.Drive(client, 127, 0, 20);
    world.Step(30); // more than half a second without input
    const Position stopped = world.PositionOf(client, own);
    TESSERA_CHECK(stopped[0] > ExampleGame::PlayerSpawn(client.ConnectionId()).first + 3.0);
    world.Step(20);
    const Position later = world.PositionOf(client, own);
    TESSERA_CHECK(Near(later[0], stopped[0], 0.011) && Near(later[2], stopped[2], 0.011));
}

TESSERA_TEST(the_player_stays_inside_the_world) {
    World world;
    ClientReplica& client = world.AddClient();
    const EntityId own = world.Join(client);
    world.Drive(client, 127, 0, 450); // far more than the 100 m to the edge
    world.Step(3);
    const Position east = world.PositionOf(client, own);
    TESSERA_CHECK(east[0] <= ExampleGame::kWorldSide + 0.011 && east[0] >= ExampleGame::kWorldSide - 0.5);
    world.Drive(client, 0, -127, 450);
    world.Step(3);
    const Position north = world.PositionOf(client, own);
    TESSERA_CHECK(north[2] >= -0.011 && north[2] <= 0.5);
    TESSERA_CHECK(north[0] >= -0.011 && north[0] <= ExampleGame::kWorldSide + 0.011);
}

TESSERA_TEST(two_players_move_independently) {
    World world;
    ClientReplica& a = world.AddClient();
    ClientReplica& b = world.AddClient();
    const EntityId a_entity = world.Join(a);
    const EntityId b_entity = world.Join(b);
    world.Step(3); // each client has to see both entities
    const Position a_start = world.PositionOf(a, a_entity);
    const Position b_start = world.PositionOf(b, b_entity);
    const uint8_t east[2] = {127, 0};
    const uint8_t south[2] = {0, 127};
    for (int i = 0; i < 45; ++i) {
        a.SendInput(east);
        b.SendInput(south);
        world.Step();
    }
    world.Step(3);
    TESSERA_CHECK(world.EntityOf(a, b.ConnectionId()) == b_entity);
    TESSERA_CHECK(world.EntityOf(b, a.ConnectionId()) == a_entity);
    const Position a_end = world.PositionOf(a, a_entity);
    const Position b_end = world.PositionOf(b, b_entity);
    TESSERA_CHECK(a_end[0] > a_start[0] + 5.0 && Near(a_end[2], a_start[2], 0.011));
    TESSERA_CHECK(b_end[2] > b_start[2] + 5.0 && Near(b_end[0], b_start[0], 0.011));
    // Both clients see each entity in the same place.
    const Position a_seen_by_b = world.PositionOf(b, a_entity);
    const Position b_seen_by_a = world.PositionOf(a, b_entity);
    TESSERA_CHECK(Distance(a_end, a_seen_by_b) < 0.5 && Distance(b_end, b_seen_by_a) < 0.5);
}

TESSERA_TEST(the_observer_follows_the_player) {
    World world;
    ClientReplica& client = world.AddClient();
    const EntityId own = world.Join(client);
    const auto [spawn_x, spawn_z] = ExampleGame::PlayerSpawn(client.ConnectionId());
    world.Drive(client, 127, 0, 60); // about 16 m
    world.Step(3);
    const Position position = world.PositionOf(client, own);
    TESSERA_CHECK(position[0] > spawn_x + 8.0);
    const std::span<const Observer> anchors =
        world.host->Replication().Relevancy().Anchors(client.ConnectionId());
    TESSERA_CHECK(anchors.size() == 1);
    if (anchors.size() == 1) {
        const double away = std::hypot(anchors[0].x - position[0], anchors[0].z - position[2]);
        TESSERA_CHECK(away <= 4.0 + kStepPerTick * 3 + 0.01); // the tolerance of the observer, and the lag
        TESSERA_CHECK(anchors[0].x > spawn_x + 8.0 && Near(anchors[0].z, spawn_z, 0.5));
    }
}

TESSERA_TEST(leaving_removes_the_entity_and_the_other_client_is_told) {
    World world;
    ClientReplica& a = world.AddClient();
    ClientReplica& b = world.AddClient();
    const EntityId a_entity = world.Join(a);
    world.Join(b);
    world.Step(3);
    TESSERA_CHECK(b.WordsOf(a_entity) != nullptr && b.VisibleCount() == 2);
    TESSERA_CHECK(world.game.PlayerEntityCount() == 2);
    a.Disconnect();
    world.Step(6);
    TESSERA_CHECK(b.WordsOf(a_entity) == nullptr);
    TESSERA_CHECK(b.VisibleCount() == 1 && world.EntityOf(b, 0).IsNull());
    TESSERA_CHECK(b.State() == ClientState::Connected);
    TESSERA_CHECK(world.game.PlayerEntityCount() == 1); // gone from the table
}

TESSERA_TEST(a_player_leaving_is_reported_to_the_others_as_a_destroyed_entity) {
    World world;
    ClientReplica& a = world.AddClient();
    Watcher& watcher = world.AddWatcher();
    const EntityId a_entity = world.Join(a);
    world.Step(6);
    TESSERA_CHECK(!watcher.SawDestroy(a_entity));
    a.Disconnect();
    world.Step(6);
    TESSERA_CHECK(watcher.SawDestroy(a_entity)); // told to the replication layer, not just taken off the grid
}

TESSERA_TEST(a_player_who_reconnects_gets_a_fresh_entity) {
    World world;
    ClientReplica& first = world.AddClient();
    const EntityId first_entity = world.Join(first);
    world.Drive(first, 127, 127, 60); // away from the spawn, with the input still running when it leaves
    first.Disconnect();
    world.Step(6);
    ClientReplica& second = world.AddClient();
    const EntityId second_entity = world.Join(second);
    TESSERA_CHECK(second.ConnectionId() == 0);          // the slot of the first player
    TESSERA_CHECK(world.game.PlayerEntityCount() == 1); // the first player's entity is gone
    TESSERA_CHECK(!second_entity.IsNull() && second_entity != first_entity);
    const auto [spawn_x, spawn_z] = ExampleGame::PlayerSpawn(second.ConnectionId());
    const Position start = world.PositionOf(second, second_entity);
    TESSERA_CHECK(Near(start[0], spawn_x, 0.011) && Near(start[2], spawn_z, 0.011));
    world.Step(30); // nothing moves it: the axes of the first player did not come along
    const Position later = world.PositionOf(second, second_entity);
    TESSERA_CHECK(Near(later[0], spawn_x, 0.011) && Near(later[2], spawn_z, 0.011));
}
