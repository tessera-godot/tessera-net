// SPDX-License-Identifier: Apache-2.0
// The body of this test is the example in docs/reference/networking.md ("Over real sockets (ENet)"). If you
// change one, change the other.
#include "tessera/net/client_replica.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/replication_server.hpp"
#include "tessera/net/spatial_grid.hpp"
#include "tessera/net/transport/enet_transport.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

using namespace tessera::net;

TESSERA_TEST(enet_reference_example) {
    // 1. The same world, schema and server as in the simulated example; only the transport changes.
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
    relevancy.types[1] = TypeRelevancy{RelevancyMode::AlwaysRelevant, 0.0f, 0.0f};

    // 2. The server listens on a UDP port (0 lets the system choose; `Port()` says which).
    EnetServerConfig listen;
    listen.bind_address = "127.0.0.1";
    listen.port = 0;
    EnetServerTransport transport(listen);

    ThreadPoolJobSystem jobs(1);
    RelevancyGroups groups;
    ServerConfig config;
    config.max_connections = 8;
    config.max_visible_per_connection = 1024;
    ReplicationServer server(config, relevancy, schema, grid, groups, jobs, transport);

    // 3. One entity that every player sees.
    const EntityId mob = table.Create();
    const auto where = table.Find(mob);
    MobColumns& columns = table.Columns(where->page);
    columns.x[where->slot] = 120.5f;
    columns.y[where->slot] = 0.0f;
    columns.z[where->slot] = -40.25f;
    table.MarkDirty(mob);
    grid.Insert(mob, 120.5f, 0.0f, -40.25f);
    server.Relevancy().OnEntityCreated(mob);

    // 4. A player connects to that port and says hello by itself.
    EnetClientConfig dial;
    dial.server_address = "127.0.0.1";
    dial.server_port = transport.Port();
    EnetClientTransport client_transport(dial);
    ClientReplica client(client_transport, schema);

    // 5. Every tick: the client reads, the server runs its network phase, and both hand the datagrams to
    //    the socket. Call `Flush` after `Poll` (the confirmation) and after `Update` (the state).
    for (uint64_t tick = 0; tick < 100 && client.WordsOf(mob) == nullptr; ++tick) {
        client.Poll();
        client_transport.Flush();
        server.Refresh(table);
        server.Update(tick);
        transport.Flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const uint32_t* quantized = client.WordsOf(mob);
    TESSERA_CHECK(client.State() == ClientState::Connected);
    TESSERA_CHECK(client.VisibleCount() == 1);
    TESSERA_CHECK(quantized != nullptr &&
                  quantized[0] == 512'050); // x = 120.5 m is 512,050 steps from -5,000 m
}
