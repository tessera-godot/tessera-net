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
#include "test_support.hpp"

using namespace tessera::net;

namespace {

struct Columns {
    float x[kPageSlots];
};
using Table = EntityTable<Columns>;

uint64_t Mix(uint64_t a, uint64_t b) {
    uint64_t z = a * 0x9E3779B97F4A7C15ull + b + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

struct Position {
    float x, y, z;
};

// Where an entity is at a given tick: a pure function of its id and the tick.
Position PositionOf(EntityId id, uint64_t tick) {
    const uint64_t h = Mix(id.raw(), 1);
    return Position{static_cast<float>(h % 2000) - 1000.0f + static_cast<float>(tick) * 0.5f,
                    static_cast<float>((h >> 16) % 200) - 100.0f, static_cast<float>((h >> 32) % 2000) - 1000.0f};
}

// A small world driven by the tick loop. Entities are created and destroyed pseudo-randomly; the
// result must not depend on the number of workers.
class Simulation {
public:
    static constexpr uint32_t kItemsPerTick = 256;

    explicit Simulation(uint32_t workers)
        : table_(TableConfig{}),
          grid_(GridConfig{16.0f, 16.0f, 16.0f}),
          jobs_(workers),
          structure_commands_(workers),
          move_commands_(workers),
          loop_(TickConfig{}, jobs_) {
        loop_.SetPhaseHandler(Phase::Simulate, [this](PhaseContext& c) { Simulate(c); });
        loop_.SetPhaseHandler(Phase::Structure, [this](PhaseContext& c) { Structure(c); });
        loop_.SetPhaseHandler(Phase::Grid, [this](PhaseContext& c) { UpdateGrid(c); });
    }

    void RunTicks(uint32_t ticks) {
        for (uint32_t i = 0; i < ticks; ++i) {
            loop_.RunTick();
        }
    }

    uint64_t StateHash() {
        uint64_t hash = 14695981039346656037ull;
        const auto feed = [&hash](uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
        for (const EntityId id : live_) {
            feed(id.raw());
        }
        feed(table_.AliveCount());
        feed(grid_.EntityCount());
        feed(grid_.CellCount());
        std::vector<EntityId> in_grid;
        grid_.QueryBox(-1.0e6f, -1.0e6f, -1.0e6f, 1.0e6f, 1.0e6f, 1.0e6f, in_grid);
        std::sort(in_grid.begin(), in_grid.end());
        for (const EntityId id : in_grid) {
            feed(id.raw());
        }
        return hash;
    }

    Table table_;
    SpatialGrid grid_;
    std::vector<EntityId> live_;
    size_t alive_end_of_simulate_ = 0;
    size_t alive_end_of_structure_ = 0;
    uint64_t last_tick_ = 0;

private:
    void Simulate(PhaseContext& context) {
        const uint64_t tick = context.tick;
        jobs_.ParallelFor(kItemsPerTick, 16, [&](uint32_t begin, uint32_t end, uint32_t worker) {
            for (uint32_t i = begin; i < end; ++i) {
                const uint64_t h = Mix(tick, i);
                if (h % 3 != 0 || live_.empty()) {
                    structure_commands_.Push(
                        worker, i, StructuralCommand{StructuralCommand::Kind::Create, 0, EntityId::Null()});
                } else {
                    const EntityId target = live_[static_cast<size_t>(h % live_.size())];
                    structure_commands_.Push(worker, i,
                                             StructuralCommand{StructuralCommand::Kind::Destroy, 0, target});
                }
            }
        });
        alive_end_of_simulate_ = table_.AliveCount();
    }

    void Structure(PhaseContext& context) {
        last_tick_ = context.tick;
        structure_commands_.ApplyAll([&](const StructuralCommand& command) {
            if (command.kind == StructuralCommand::Kind::Create) {
                const EntityId id = table_.Create();
                if (!id.IsNull()) {
                    live_index_[id.raw()] = live_.size();
                    live_.push_back(id);
                    const Position p = PositionOf(id, context.tick);
                    grid_.Insert(id, p.x, p.y, p.z);
                }
            } else if (table_.Destroy(command.id)) {
                const size_t position = live_index_[command.id.raw()];
                const EntityId moved = live_.back();
                live_[position] = moved;
                live_index_[moved.raw()] = position;
                live_.pop_back();
                live_index_.erase(command.id.raw());
                grid_.Remove(command.id);
            }
        });
        alive_end_of_structure_ = table_.AliveCount();
    }

    void UpdateGrid(PhaseContext& context) {
        const uint64_t tick = context.tick;
        jobs_.ParallelFor(static_cast<uint32_t>(live_.size()), 256,
                          [&](uint32_t begin, uint32_t end, uint32_t worker) {
                              for (uint32_t i = begin; i < end; ++i) {
                                  const EntityId id = live_[i];
                                  const Position p = PositionOf(id, tick);
                                  if (grid_.WouldChangeCell(id, p.x, p.y, p.z)) {
                                      move_commands_.Push(worker, i, PendingMove{id, p.x, p.y, p.z});
                                  }
                              }
                          });
        std::vector<PendingMove> pending;
        move_commands_.ApplyAll([&](const PendingMove& move) { pending.push_back(move); });
        grid_.ApplyMoves(pending);
    }

    ThreadPoolJobSystem jobs_;
    CommandBuffers<StructuralCommand> structure_commands_;
    CommandBuffers<PendingMove> move_commands_;
    TickLoop loop_;
    std::unordered_map<uint64_t, size_t> live_index_;
};

}  // namespace

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
