// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/entity_id.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/relevancy/relevancy_config.hpp"
#include "tessera/net/relevancy/relevancy_engine.hpp"
#include "tessera/net/relevancy/relevancy_groups.hpp"
#include "tessera/net/spatial_grid.hpp"

#include <cstdint>
#include <vector>

namespace relevancy_test {

using namespace tessera::net;

// Entity types used by the tests.
inline constexpr uint32_t kSpatialType = 0; // range 100
inline constexpr uint32_t kGlobalType = 1;  // always relevant
inline constexpr uint32_t kOwnedType = 2;   // owner only
inline constexpr uint32_t kFarType = 3;     // spatial, range 500
inline constexpr uint32_t kNoRangeType = 4; // spatial, range 0

inline EntityId MakeId(uint32_t type, uint32_t index) {
    return EntityId::Make(0, type, index, 0);
}

inline RelevancyConfig MakeConfig(uint32_t interval_ticks = 3) {
    RelevancyConfig config;
    config.recalc_interval_ticks = interval_ticks;
    config.types[kSpatialType] = TypeRelevancy{RelevancyMode::Spatial, 100.0f, 0.0f};
    config.types[kGlobalType] = TypeRelevancy{RelevancyMode::AlwaysRelevant, 0.0f, 0.0f};
    config.types[kOwnedType] = TypeRelevancy{RelevancyMode::OwnerOnly, 0.0f, 0.0f};
    config.types[kFarType] = TypeRelevancy{RelevancyMode::Spatial, 500.0f, 0.0f};
    config.types[kNoRangeType] = TypeRelevancy{RelevancyMode::Spatial, 0.0f, 0.0f};
    return config;
}

/// Everything an engine needs, owned in one place.
struct Fixture {
    explicit Fixture(const RelevancyConfig& config, uint32_t workers = 1)
        : jobs(workers), engine(config, grid, groups, jobs) {}

    SpatialGrid grid{GridConfig{64.0f, 64.0f, 64.0f}};
    RelevancyGroups groups;
    ThreadPoolJobSystem jobs;
    RelevancyEngine engine;
};

/// Counts the events of one kind in a connection's list.
inline size_t CountEvents(const ConnectionEvents& events, VisibilityEvent::Kind kind) {
    size_t count = 0;
    for (const VisibilityEvent& e : events.events) {
        if (e.kind == kind) {
            ++count;
        }
    }
    return count;
}

} // namespace relevancy_test
