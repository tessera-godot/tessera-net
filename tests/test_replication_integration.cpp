// SPDX-License-Identifier: Apache-2.0
// Replicated state driven by the real relevancy engine: players move, entities move, appear and disappear.
#include "replication_test_support.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/relevancy/relevancy_config.hpp"
#include "tessera/net/relevancy/relevancy_engine.hpp"
#include "tessera/net/relevancy/relevancy_groups.hpp"
#include "tessera/net/replication/receiver_state.hpp"
#include "tessera/net/replication/replication_engine.hpp"
#include "tessera/net/spatial_grid.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <vector>

using namespace tessera::net;
using namespace replication_test;

namespace {

using Table = EntityTable<TestColumns>;

constexpr uint32_t kIds = 1000;

RelevancyConfig MakeRelevancyConfig(uint32_t connections) {
    RelevancyConfig config;
    config.max_connections = connections;
    config.max_visible_per_connection = kIds;
    config.types[kMobType] = TypeRelevancy{RelevancyMode::Spatial, 150.0f, 10.0f};
    return config;
}

ReplicationConfig MakeReplicationConfig(uint32_t connections) {
    ReplicationConfig config;
    config.max_connections = connections;
    config.max_visible_per_connection = kIds;
    return config;
}

// Everything a server and its players need, in one place.
struct Sim {
    explicit Sim(uint32_t connections)
        : connection_count(connections), grid(GridConfig{64.0f, 64.0f, 64.0f}), jobs(2),
          relevancy(MakeRelevancyConfig(connections), grid, groups, jobs),
          replication(schema, MakeReplicationConfig(connections), jobs), table(TableConfig{0, kMobType}) {
        observers.assign(connections, Observer{0.0f, 0.0f, 0.0f});
        for (ConnectionId c = 0; c < connections; ++c) {
            relevancy.AddConnection(c);
            replication.AddConnection(c);
            clients.push_back(std::make_unique<ReceiverState>(schema, kIds, 32));
            relevancy.SetObservers(c, std::span<const Observer>(&observers[c], 1));
        }
    }

    EntityId AddMob(float x, float z) {
        const EntityId id = table.Create();
        const auto where = table.Find(id);
        TestColumns& columns = table.Columns(where->page);
        columns.x[where->slot] = x;
        columns.y[where->slot] = 0.0f;
        columns.z[where->slot] = z;
        columns.yaw[where->slot] = 0.5f;
        columns.health[where->slot] = 50;
        table.MarkDirty(id);
        grid.Insert(id, x, 0.0f, z);
        return id;
    }

    void MoveMob(EntityId id, float x, float z, std::vector<PendingMove>& moves) {
        const auto where = table.Find(id);
        TestColumns& columns = table.Columns(where->page);
        columns.x[where->slot] = x;
        columns.z[where->slot] = z;
        table.MarkDirty(id);
        moves.push_back(PendingMove{id, x, 0.0f, z});
    }

    void DestroyMob(EntityId id) {
        grid.Remove(id);
        table.Destroy(id);
        relevancy.OnEntityDestroyed(id);
    }

    // One tick of the Network phase for everyone: relevancy, then replication, then the players' side. The
    // players confirm every buffer one tick late.
    void Tick(uint64_t tick, uint32_t* complete_records = nullptr) {
        relevancy.Update(tick);
        for (ConnectionId c = 0; c < connection_count; ++c) {
            const std::vector<VisibilityEvent>& events = relevancy.Events(c).events;
            replication.ApplyVisibility(c, events);
            for (const VisibilityEvent& event : events) {
                if (event.kind == VisibilityEvent::Kind::Enter) {
                    TESSERA_CHECK(clients[c]->OnEnter(event.compact_id, event.id, tick));
                    ++enters;
                } else {
                    clients[c]->OnLeave(event.compact_id);
                }
            }
            clients[c]->AdvanceEvents(tick);
            relevancy.ClearEvents(c);
        }
        replication.Refresh(table);
        replication.Update(tick);
        if (complete_records != nullptr) {
            *complete_records = replication.LastStats().complete_records;
        }
        for (ConnectionId c = 0; c < connection_count; ++c) {
            TESSERA_CHECK(clients[c]->Apply(replication.Output(c), tick) == DecodeStatus::Ok);
            if (tick > 1) {
                replication.Confirm(c, tick - 1);
            }
        }
    }

    // What every player's receiver holds is exactly what the relevancy engine says they can see, with the
    // state the server has for each of those entities.
    void CheckPlayersAreUpToDate(const std::vector<EntityId>& candidates) {
        for (ConnectionId c = 0; c < connection_count; ++c) {
            uint32_t known = 0;
            for (uint32_t id = 0; id < kIds; ++id) {
                known += clients[c]->Current(id) != nullptr ? 1 : 0;
            }
            TESSERA_CHECK(known == relevancy.VisibleCount(c));
            for (const EntityId entity : candidates) {
                const uint32_t compact_id = relevancy.CompactIdOf(c, entity);
                if (compact_id == kInvalidCompactId) {
                    continue;
                }
                const uint32_t* held = clients[c]->Current(compact_id);
                const uint32_t* server = replication.Store().Find(entity);
                TESSERA_CHECK(held != nullptr && server != nullptr && std::equal(server, server + 9, held));
            }
        }
    }

    uint32_t connection_count;
    ReplicationSchema schema = MakeTestSchema();
    SpatialGrid grid;
    RelevancyGroups groups;
    ThreadPoolJobSystem jobs;
    RelevancyEngine relevancy;
    ReplicationEngine replication;
    Table table;
    std::vector<std::unique_ptr<ReceiverState>> clients;
    std::vector<Observer> observers;
    uint64_t enters = 0;
};

} // namespace

TESSERA_TEST(relevancy_events_drive_the_replication_of_moving_entities) {
    Sim sim(3);
    std::mt19937 random(7);
    std::uniform_real_distribution<float> place(0.0f, 600.0f);
    std::uniform_real_distribution<float> nudge(-5.0f, 5.0f);
    std::uniform_real_distribution<float> walk(-8.0f, 8.0f);
    std::vector<EntityId> mobs;
    for (int i = 0; i < 80; ++i) {
        mobs.push_back(sim.AddMob(place(random), place(random)));
    }
    for (ConnectionId c = 0; c < 3; ++c) {
        sim.observers[c] = Observer{place(random), 0.0f, place(random)};
    }
    uint64_t tick = 1;
    for (; tick <= 60; ++tick) {
        std::vector<PendingMove> moves;
        for (const EntityId id : mobs) {
            if (random() % 4 == 0) {
                const auto where = sim.table.Find(id);
                const TestColumns& columns = sim.table.Columns(where->page);
                sim.MoveMob(id, columns.x[where->slot] + nudge(random),
                            columns.z[where->slot] + nudge(random), moves);
            }
        }
        sim.grid.ApplyMoves(moves);
        if (tick % 10 == 0) { // an entity is destroyed and another one appears
            const size_t victim = random() % mobs.size();
            sim.DestroyMob(mobs[victim]);
            mobs[victim] = sim.AddMob(place(random), place(random));
        }
        for (ConnectionId c = 0; c < 3; ++c) {
            sim.observers[c].x += walk(random);
            sim.observers[c].z += walk(random);
            sim.relevancy.SetObservers(c, std::span<const Observer>(&sim.observers[c], 1));
        }
        sim.Tick(tick);
    }
    for (int extra = 0; extra < 4; ++extra, ++tick) { // nothing moves, so everything settles
        sim.Tick(tick);
    }
    TESSERA_CHECK(sim.enters > 50); // players really did see entities come and go
    for (ConnectionId c = 0; c < 3; ++c) {
        TESSERA_CHECK(sim.relevancy.VisibleCount(c) > 0);
    }
    sim.CheckPlayersAreUpToDate(mobs);
}

TESSERA_TEST(entities_that_leave_and_return_get_a_complete_record) {
    Sim sim(1);
    const EntityId mob = sim.AddMob(0.0f, 0.0f);
    uint32_t complete = 0;
    sim.Tick(1, &complete);
    TESSERA_CHECK(complete == 1 && sim.relevancy.VisibleCount(0) == 1);
    sim.Tick(2, &complete);
    sim.Tick(3, &complete);
    TESSERA_CHECK(complete == 0); // seen and confirmed: nothing more to write

    sim.observers[0] = Observer{5000.0f, 0.0f, 0.0f}; // far away
    sim.relevancy.SetObservers(0, std::span<const Observer>(&sim.observers[0], 1));
    sim.Tick(4, &complete);
    TESSERA_CHECK(sim.relevancy.VisibleCount(0) == 0 && sim.clients[0]->Current(0) == nullptr);
    sim.Tick(5, &complete);
    TESSERA_CHECK(complete == 0);

    sim.observers[0] = Observer{0.0f, 0.0f, 0.0f}; // and back
    sim.relevancy.SetObservers(0, std::span<const Observer>(&sim.observers[0], 1));
    sim.Tick(6, &complete);
    TESSERA_CHECK(complete == 1); // everything again, never a delta against what the player dropped
    sim.Tick(7);
    sim.Tick(8);
    sim.CheckPlayersAreUpToDate({mob});
}
