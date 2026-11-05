// SPDX-License-Identifier: Apache-2.0
// Integration and property tests: the table, grid, command buffers, jobs and tick loop working together.
#include <algorithm>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tessera/net/command_buffer.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/spatial_grid.hpp"
#include "tessera/net/tick_loop.hpp"
#include "simulation_harness.hpp"
#include "test_support.hpp"

using namespace tessera::net;
using namespace tessera::net::harness;

TESSERA_TEST(random_operations_match_reference_model) {
    Table table(TableConfig{});
    std::unordered_set<uint64_t> model;  // raw ids that should be alive
    std::vector<EntityId> alive;
    std::vector<EntityId> history;  // every id ever issued, to probe stale ids
    std::mt19937 rng(12345);
    int mismatches = 0;
    for (int op = 0; op < 200000; ++op) {
        const uint32_t kind = rng() % 3;
        if (kind == 0 || alive.empty()) {
            const EntityId id = table.Create();
            model.insert(id.raw());
            alive.push_back(id);
            history.push_back(id);
        } else if (kind == 1) {
            const size_t pick = rng() % alive.size();
            const EntityId id = alive[pick];
            if (!table.Destroy(id)) {
                ++mismatches;
            }
            model.erase(id.raw());
            alive[pick] = alive.back();
            alive.pop_back();
        } else {
            const EntityId id = history[rng() % history.size()];
            if (table.IsAlive(id) != (model.count(id.raw()) == 1)) {
                ++mismatches;
            }
        }
        if (op % 1000 == 0 && table.AliveCount() != model.size()) {
            ++mismatches;
        }
    }
    TESSERA_CHECK(mismatches == 0);
    TESSERA_CHECK(table.AliveCount() == model.size());
}

TESSERA_TEST(tick_pipeline_applies_structural_commands_in_phase_three) {
    Simulation simulation(4);
    simulation.RunTicks(1);
    TESSERA_CHECK(simulation.alive_end_of_simulate_ == 0);  // queued, not applied yet
    TESSERA_CHECK(simulation.alive_end_of_structure_ == Simulation::kItemsPerTick);
}

TESSERA_TEST(structure_hash_is_identical_with_1_and_8_workers) {
    Simulation one(1);
    Simulation eight(8);
    one.RunTicks(300);
    eight.RunTicks(300);
    TESSERA_CHECK(one.live_.size() == eight.live_.size());
    TESSERA_CHECK(one.live_.size() > 0);
    TESSERA_CHECK(one.StateHash() == eight.StateHash());
}

TESSERA_TEST(grid_follows_table_after_moves) {
    Simulation simulation(4);
    simulation.RunTicks(100);
    TESSERA_CHECK(simulation.grid_.EntityCount() == simulation.table_.AliveCount());
    size_t in_wrong_cell = 0;
    for (const EntityId id : simulation.live_) {
        const Position p = PositionOf(id, simulation.last_tick_);
        if (simulation.grid_.WouldChangeCell(id, p.x, p.y, p.z)) {
            ++in_wrong_cell;
        }
    }
    TESSERA_CHECK(in_wrong_cell == 0);
}
