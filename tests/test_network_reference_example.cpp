// SPDX-License-Identifier: Apache-2.0
// The body of this test is the example in docs/reference/networking.md. If you change one, change the other.
#include "tessera/net/client_replica.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/replication_server.hpp"
#include "tessera/net/spatial_grid.hpp"
#include "tessera/net/transport/simulated_network.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <span>

using namespace tessera::net;

TESSERA_TEST(networking_reference_example) {
    // 1. The columns of one entity type, as its entity table keeps them.
    struct MobColumns {
        float x[kPageSlots];
        float y[kPageSlots];
        float z[kPageSlots];
        float yaw[kPageSlots];
        int32_t health[kPageSlots];
    };

    // 2. Say which properties of type 1 are replicated, and how precisely.
    ReplicationSchema schema;
    schema.AddType(
        1, {PropertyDescriptor::Position(TESSERA_COLUMN(MobColumns, x), TESSERA_COLUMN(MobColumns, y),
                                         TESSERA_COLUMN(MobColumns, z), 0.01f,
                                         5000.0f), // 1 cm steps, up to 5 km from the origin
            PropertyDescriptor::Angle(TESSERA_COLUMN(MobColumns, yaw), 10), // 1,024 steps around the circle
            PropertyDescriptor::Integer(TESSERA_COLUMN(MobColumns, health), true, 0, 1000)});

    // 3. The world: a table for the entities and a grid that knows where they are.
    EntityTable<MobColumns> table(TableConfig{0, 1});
    SpatialGrid grid(GridConfig{64.0f, 64.0f, 64.0f});

    // 4. Players see entities of type 1 up to 150 m away (and keep them until 160 m).
    RelevancyConfig relevancy;
    relevancy.types[1] = TypeRelevancy{RelevancyMode::Spatial, 150.0f, 10.0f};

    // 5. A network that loses 5% of what it carries and delays every message by up to two ticks. A real game
    //    gives the server a transport that talks to real sockets instead; the rest stays the same.
    NetworkConditions conditions;
    conditions.loss = 0.05;
    conditions.max_delay_ticks = 2;
    SimulatedNetwork network(conditions);

    // 6. The server. It owns the relevancy and replication engines and does the whole Network phase.
    ThreadPoolJobSystem jobs(ThreadPoolJobSystem::DefaultWorkerCount());
    RelevancyGroups groups;
    ServerConfig config;
    config.max_connections = 8;
    config.max_visible_per_connection = 1024;
    ReplicationServer server(config, relevancy, schema, grid, groups, jobs, network.Server());

    // 7. Tell the relevancy engine where each player is looking from, as soon as the player is connected.
    Observer eye{0.0f, 0.0f, 0.0f};
    server.SetCallbacks(
        [&](ConnectionId id) { server.Relevancy().SetObservers(id, std::span<const Observer>(&eye, 1)); },
        [](ConnectionId) {});

    // 8. Create an entity, write its columns, flag it as changed and tell the grid where it is.
    const EntityId mob = table.Create();
    const auto where = table.Find(mob);
    MobColumns& columns = table.Columns(where->page);
    columns.x[where->slot] = 120.5f;
    columns.y[where->slot] = 0.0f;
    columns.z[where->slot] = -40.25f;
    columns.yaw[where->slot] = 1.0f;
    columns.health[where->slot] = 80;
    table.MarkDirty(mob);
    grid.Insert(mob, 120.5f, 0.0f, -40.25f);

    // 9. A player: a client on the other end of the network. It connects and says hello by itself.
    ClientReplica client(*network.AddClient(), schema);

    // 10. Every tick: the network delivers what is due, the client reads what arrived, and the server runs
    // its
    //     Network phase (quantize what changed, then update). Both sides confirm and resend by themselves.
    for (uint64_t tick = 0; tick < 60; ++tick) {
        network.Advance(tick);
        client.Poll();
        server.Refresh(table);
        server.Update(tick);
    }

    const uint32_t* quantized = client.WordsOf(mob); // the position, angle and health as whole numbers
    TESSERA_CHECK(client.State() == ClientState::Connected);
    TESSERA_CHECK(client.VisibleCount() == 1);
    TESSERA_CHECK(quantized != nullptr &&
                  quantized[0] == 512'050); // x = 120.5 m is 512,050 steps from -5,000 m
}
