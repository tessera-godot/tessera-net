// SPDX-License-Identifier: Apache-2.0
// Everything below this comment is the example in docs/reference/networking.md ("Running a server"). If you change
// one, change the other.
#include "tessera/net/client_replica.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/transport/simulated_network.hpp"
#include "tessera/server/game_logic.hpp"
#include "tessera/server/server_host.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <memory>
#include <string>

using namespace tessera::net;
namespace server = tessera::server;

// 1. The game: one kind of entity with a position, and one entity that stays where it is.
struct MobColumns {
    float x[kPageSlots];
    float y[kPageSlots];
    float z[kPageSlots];
};

class MiniGame final : public server::IGameLogic {
public:
    MiniGame() {
        schema_.AddType(1, {PropertyDescriptor::Position(TESSERA_COLUMN(MobColumns, x),
                                                         TESSERA_COLUMN(MobColumns, y),
                                                         TESSERA_COLUMN(MobColumns, z), 0.01f, 5000.0f)});
    }

    // What the host needs before it can build anything: the schema, and who sees what.
    server::WorldSetup DescribeWorld() override {
        server::WorldSetup setup;
        setup.schema = &schema_;
        setup.relevancy.types[1] = TypeRelevancy{RelevancyMode::AlwaysRelevant, 0.0f, 0.0f};
        return setup;
    }

    // Once, before the first tick: make the table, with the shard id of the server in its ids, and one entity.
    void OnStart(server::ServerContext& context) override {
        context_ = &context;
        table_ = std::make_unique<EntityTable<MobColumns>>(TableConfig{context.shard_id, 1});
        mob = table_->Create();
        const auto where = table_->Find(mob);
        MobColumns& columns = table_->Columns(where->page);
        columns.x[where->slot] = 120.5f;
        columns.y[where->slot] = 0.0f;
        columns.z[where->slot] = -40.25f;
        table_->MarkDirty(mob);
        context.grid.Insert(mob, 120.5f, 0.0f, -40.25f);
        context.replication.Relevancy().OnEntityCreated(mob);
    }

    // Every tick: simulate, then hand the changes of each table to the replication layer.
    void OnTick(double /*dt_seconds*/) override {
        context_->replication.Refresh(*table_);
    }

    void OnPlayerConnected(ConnectionId player) override {
        context_->log.Info("player " + std::to_string(player) + " is in");
    }

    void OnShutdown() override {
        stopped = true;
    }

    const ReplicationSchema& Schema() const {
        return schema_;
    }

    EntityId mob = EntityId::Null();
    bool stopped = false;

private:
    ReplicationSchema schema_;
    server::ServerContext* context_ = nullptr;
    std::unique_ptr<EntityTable<MobColumns>> table_;
};

TESSERA_TEST(server_reference_example) {
    MiniGame game;

    // 2. The settings. A real server reads them with `ResolveConfig` from a file and the command line.
    server::ServerConfig config;
    config.shard_id = 5;
    config.tick_rate_hz = 30;
    config.max_players = 8;

    // 3. A network in memory and a logger that says nothing. A real server gives the host an ENet transport and a
    //    logger that writes to the standard output (`server::StdoutSink()`).
    SimulatedNetwork network(NetworkConditions{});
    server::Logger log(server::LogLevel::Info, [](const std::string&) {});

    // 4. The host builds the world the game describes and owns the tick loop.
    server::ServerHost host(config, game, network.Server(), log);

    // 5. A player: a client on the other end of the network.
    ClientReplica client(*network.AddClient(), game.Schema());

    // 6. A real server calls `host.Run()`, which ticks at the configured rate until a stop is requested. Here the
    //    ticks are run by hand, so the test does not have to wait.
    for (uint64_t tick = 0; tick < 20; ++tick) {
        network.Advance(tick);
        client.Poll();
        host.Step();
    }

    // 7. Stopping hands what is pending to the transport and calls `OnShutdown` once.
    host.Shutdown();

    const uint32_t* quantized = client.WordsOf(game.mob);
    TESSERA_CHECK(client.State() == ClientState::Connected);
    TESSERA_CHECK(client.VisibleCount() == 1);
    TESSERA_CHECK(game.mob.shard() == 5); // the shard id of the server is in the id of the entity
    // x = 120.5 m is 512,050 steps from -5,000 m
    TESSERA_CHECK(quantized != nullptr && quantized[0] == 512'050);
    TESSERA_CHECK(game.stopped);
}
