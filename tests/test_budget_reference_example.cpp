// SPDX-License-Identifier: Apache-2.0
// The body of this test is the example in the budget section of docs/reference/replicated-state.md. If you
// change one, change the other.
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

TESSERA_TEST(budget_reference_example) {
    struct MobColumns {
        float x[kPageSlots];
        float y[kPageSlots];
        float z[kPageSlots];
    };

    ReplicationSchema schema;
    schema.AddType(1,
                   {PropertyDescriptor::Position(TESSERA_COLUMN(MobColumns, x), TESSERA_COLUMN(MobColumns, y),
                                                 TESSERA_COLUMN(MobColumns, z), 0.01f, 5000.0f)});

    EntityTable<MobColumns> table(TableConfig{0, 1});
    SpatialGrid grid(GridConfig{64.0f, 64.0f, 64.0f});
    RelevancyConfig relevancy;
    relevancy.types[1] = TypeRelevancy{RelevancyMode::Spatial, 150.0f, 10.0f};

    NetworkConditions conditions;
    conditions.loss = 0.05;
    conditions.max_delay_ticks = 2;
    SimulatedNetwork network(conditions);

    // The budget. Each player gets 8 kilobits a second, with no room to save up for bursts (the peak is the
    // rate), in buffers of at most 600 bytes. An entity that was sent and not confirmed waits for the
    // player's round trip plus one tick before it is sent again, and entities of type 1 count four times as
    // much as the rest when there is not room for everything.
    ThreadPoolJobSystem jobs(ThreadPoolJobSystem::DefaultWorkerCount());
    RelevancyGroups groups;
    ServerConfig config;
    config.max_connections = 8;
    config.max_visible_per_connection = 1024;
    config.rate_kbps = 8;
    config.peak_kbps = 8;
    config.max_buffer_bytes = 600;
    config.resend_margin_ticks = 1;
    config.type_weight[1] = 4;
    ReplicationServer server(config, relevancy, schema, grid, groups, jobs, network.Server());

    Observer eye{0.0f, 0.0f, 0.0f};
    server.SetCallbacks(
        [&](ConnectionId id) { server.Relevancy().SetObservers(id, std::span<const Observer>(&eye, 1)); },
        [](ConnectionId) {});

    // 300 mobs around the player: more than the budget can send in the first second.
    for (uint32_t i = 0; i < 300; ++i) {
        const EntityId mob = table.Create();
        const auto where = table.Find(mob);
        MobColumns& columns = table.Columns(where->page);
        columns.x[where->slot] = static_cast<float>(i % 20) * 5.0f;
        columns.y[where->slot] = 0.0f;
        columns.z[where->slot] = static_cast<float>(i / 20) * 5.0f;
        table.MarkDirty(mob);
        grid.Insert(mob, columns.x[where->slot], 0.0f, columns.z[where->slot]);
    }

    ClientReplica client(*network.AddClient(), schema);
    uint32_t seen_after_one_second = 0;
    for (uint64_t tick = 0; tick < 300; ++tick) {
        network.Advance(tick);
        client.Poll();
        server.Refresh(table);
        server.Update(tick);
        if (tick == 29) {
            client.ForEachEntity([&](EntityId, const uint32_t*) { ++seen_after_one_second; });
        }
    }

    // The budget held some entities back at first, and in the end the player has all of them.
    uint32_t seen = 0;
    client.ForEachEntity([&](EntityId, const uint32_t*) { ++seen; });
    TESSERA_CHECK(seen_after_one_second < 300);
    TESSERA_CHECK(seen == 300);
    const uint32_t wait = server.Replication().ResendAfterTicks(client.ConnectionId()); // measured, in ticks
    TESSERA_CHECK(wait >= config.min_resend_ticks && wait <= config.max_resend_ticks);
}
