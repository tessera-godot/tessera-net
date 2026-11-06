// SPDX-License-Identifier: Apache-2.0
// The code of the example in docs/reference/relevancy.md, kept here so it is compiled and run with
// every build. If you change one, change the other.
#include "tessera/net/job_system.hpp"
#include "tessera/net/relevancy/relevancy_engine.hpp"
#include "tessera/net/spatial_grid.hpp"
#include "test_support.hpp"

#include <span>

using namespace tessera::net;

TESSERA_TEST(reference_example_basic_flow) {
    // 1. Say how far each type of entity can be seen.
    RelevancyConfig config;
    config.max_connections = 64;
    config.types[1] = {RelevancyMode::Spatial, 150.0f, 10.0f};     // type 1: crates, seen from 150 m
    config.types[2] = {RelevancyMode::AlwaysRelevant, 0.0f, 0.0f}; // type 2: the world clock
    config.types[3] = {RelevancyMode::OwnerOnly, 0.0f, 0.0f};      // type 3: a player's backpack

    SpatialGrid grid(GridConfig{64.0f, 64.0f, 64.0f});
    RelevancyGroups groups;
    ThreadPoolJobSystem jobs(ThreadPoolJobSystem::DefaultWorkerCount());
    RelevancyEngine engine(config, grid, groups, jobs);

    // 2. A player connects and looks from one point.
    engine.AddConnection(0);
    const Observer camera{10.0f, 0.0f, 10.0f};
    engine.SetObservers(0, std::span<const Observer>(&camera, 1));

    // 3. The world has a crate nearby and a clock.
    const EntityId crate = EntityId::Make(0, 1, 7, 0);
    grid.Insert(crate, 30.0f, 0.0f, 30.0f);
    const EntityId clock = EntityId::Make(0, 2, 1, 0);
    engine.OnEntityCreated(clock);

    // 4. Every tick, in the Network phase:
    const uint64_t tick = 0;
    engine.Update(tick);
    size_t entered = 0;
    for (const VisibilityEvent& event : engine.Events(0).events) {
        if (event.kind == VisibilityEvent::Kind::Enter) {
            ++entered; // start sending event.id using event.compact_id
        }
        // Leave and Destroy: stop sending it.
    }
    engine.ClearEvents(0);

    TESSERA_CHECK(entered == 2);
    TESSERA_CHECK(engine.CompactIdOf(0, crate) != kInvalidCompactId);
    TESSERA_CHECK(engine.CompactIdOf(0, clock) != kInvalidCompactId);
    TESSERA_CHECK(engine.Events(0).events.empty());
}

TESSERA_TEST(reference_example_groups) {
    RelevancyConfig config;
    config.types[1] = {RelevancyMode::Spatial, 150.0f, 10.0f};
    SpatialGrid grid(GridConfig{64.0f, 64.0f, 64.0f});
    RelevancyGroups groups;
    ThreadPoolJobSystem jobs(1);
    RelevancyEngine engine(config, grid, groups, jobs);
    engine.AddConnection(0);
    engine.AddConnection(1);
    const Observer camera{0.0f, 0.0f, 0.0f};
    engine.SetObservers(0, std::span<const Observer>(&camera, 1));
    engine.SetObservers(1, std::span<const Observer>(&camera, 1));
    const EntityId crate = EntityId::Make(0, 1, 7, 0);
    grid.Insert(crate, 30.0f, 0.0f, 30.0f);

    // Hide the crate from everyone except connection 0.
    const GroupId secret = groups.CreateGroup(GroupKind::Exclusion);
    groups.AddMember(secret, crate);
    groups.SetStatus(secret, 0, true); // connection 0 is allowed to see it

    engine.Update(0);
    engine.Update(1);
    TESSERA_CHECK(engine.CompactIdOf(0, crate) != kInvalidCompactId);
    TESSERA_CHECK(engine.CompactIdOf(1, crate) == kInvalidCompactId);
}
