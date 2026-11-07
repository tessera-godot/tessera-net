// SPDX-License-Identifier: Apache-2.0
// Acceptance benchmark for the relevancy engine. The main scenario has 128 connections and one million
// entities in a 10 km x 10 km world, 700 of them moving every tick, with about 1,000 to 2,000 entities
// visible to each connection. A second, smaller scenario is reported for information.
#include "bench_relevancy.hpp"

#include "bench_memory.hpp"
#include "bench_report.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/relevancy/relevancy_engine.hpp"
#include "tessera/net/spatial_grid.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <random>
#include <span>
#include <vector>

using namespace tessera::net;
using bench::Clock;
using bench::Info;
using bench::Median;
using bench::Ms;
using bench::Report;
using bench::ReportBool;
using bench::ResidentBytes;

namespace {

// Regression limits for the main scenario: about twice what the engine measures, so that run-to-run
// variation does not fail the benchmark but a real slowdown does. The cost goals were 1.5 ms per tick with
// one thread and 0.4 ms with eight workers; the engine meets both.
constexpr double kLimitOneThreadMs = 2.0;
constexpr double kLimitEightWorkersMs = 0.7;

constexpr int kWarmupTicks = 9;
constexpr int kRuns = 5;
constexpr int kTicksPerRun = 30;

// Four spatial types; most entities are small things seen from close by, a few are seen from far.
constexpr float kRanges[4] = {100.0f, 250.0f, 400.0f, 600.0f};
constexpr float kMargin = 8.0f;

// Movement at 30 ticks per second. Observers walk at 5 m/s, and a share of them drive at 25 m/s;
// the entities that move each tick take a step of 0.2 m (about 6 m/s).
constexpr float kPi = 3.14159265f;
constexpr float kWalkStep = 5.0f / 30.0f;
constexpr float kFastStep = 25.0f / 30.0f;
constexpr float kEntityStep = 0.2f;

struct Scenario {
    uint32_t connections;
    uint32_t entities;
    float world_size; // side of the square world, in metres
    uint32_t moving_per_tick;
};

// The reference scenario and a smaller one with the same entity density (10,000 per square km).
constexpr Scenario kMain{128, 1'000'000, 10'000.0f, 700};
constexpr Scenario kSmall{32, 250'000, 5'000.0f, 175};

struct World {
    Scenario scenario;
    std::vector<EntityId> ids;
    std::vector<float> x, y, z;
    std::mt19937 rng{2024};

    // The old movement, kept for information: entities and observers jump tens of metres per tick, far
    // more than any game does, which makes every visible set change a lot.
    bool stress_movement = false;
    float fast_fraction = 0.1f; // share of observers that move at vehicle speed
    std::vector<float> heading; // per observer, in radians
    std::vector<float> step;    // per observer, metres per tick

    explicit World(Scenario s) : scenario(s) {}

    float Random(float low, float high) {
        return std::uniform_real_distribution<float>(low, high)(rng);
    }

    float Clamp(float value) const {
        return std::min(std::max(value, 0.0f), scenario.world_size);
    }

    void Build(SpatialGrid& grid) {
        ids.reserve(scenario.entities);
        x.reserve(scenario.entities);
        y.reserve(scenario.entities);
        z.reserve(scenario.entities);
        for (uint32_t i = 0; i < scenario.entities; ++i) {
            const uint32_t roll = static_cast<uint32_t>(rng() % 100);
            const uint32_t type = roll < 60 ? 0 : roll < 85 ? 1 : roll < 95 ? 2 : 3;
            ids.push_back(EntityId::Make(0, type, i, 0));
            x.push_back(Random(0.0f, scenario.world_size));
            y.push_back(Random(0.0f, 50.0f));
            z.push_back(Random(0.0f, scenario.world_size));
            grid.Insert(ids.back(), x.back(), y.back(), z.back());
        }
    }

    void MoveSome(SpatialGrid& grid) {
        std::vector<PendingMove> moves;
        moves.reserve(scenario.moving_per_tick);
        for (uint32_t m = 0; m < scenario.moving_per_tick; ++m) {
            const uint32_t i = static_cast<uint32_t>(rng() % scenario.entities);
            if (stress_movement) {
                x[i] = Clamp(x[i] + Random(-30.0f, 30.0f));
                z[i] = Clamp(z[i] + Random(-30.0f, 30.0f));
            } else {
                const float angle = Random(0.0f, 2.0f * kPi);
                x[i] = Clamp(x[i] + std::cos(angle) * kEntityStep);
                z[i] = Clamp(z[i] + std::sin(angle) * kEntityStep);
            }
            moves.push_back(PendingMove{ids[i], x[i], y[i], z[i]});
        }
        grid.ApplyMoves(moves);
    }

    std::vector<Observer> PlaceObservers() {
        std::vector<Observer> eyes(scenario.connections);
        const float margin = scenario.world_size / 10.0f;
        for (Observer& eye : eyes) {
            eye = Observer{Random(margin, scenario.world_size - margin), 25.0f,
                           Random(margin, scenario.world_size - margin)};
        }
        heading.resize(eyes.size());
        step.resize(eyes.size());
        for (size_t c = 0; c < eyes.size(); ++c) {
            heading[c] = Random(0.0f, 2.0f * kPi);
            step[c] = Random(0.0f, 1.0f) < fast_fraction ? kFastStep : kWalkStep;
        }
        return eyes;
    }

    // Moves every observer by one tick. Walkers and drivers keep a heading that turns a few degrees per
    // tick and turn around at the edge of the world.
    void StepObservers(std::vector<Observer>& eyes) {
        for (size_t c = 0; c < eyes.size(); ++c) {
            if (stress_movement) {
                eyes[c].x = Clamp(eyes[c].x + Random(-10.0f, 10.0f));
                eyes[c].z = Clamp(eyes[c].z + Random(-10.0f, 10.0f));
                continue;
            }
            heading[c] += Random(-0.05f, 0.05f);
            float nx = eyes[c].x + std::cos(heading[c]) * step[c];
            float nz = eyes[c].z + std::sin(heading[c]) * step[c];
            if (nx < 0.0f || nx > scenario.world_size || nz < 0.0f || nz > scenario.world_size) {
                heading[c] += kPi;
                nx = Clamp(nx);
                nz = Clamp(nz);
            }
            eyes[c].x = nx;
            eyes[c].z = nz;
        }
    }
};

RelevancyConfig MakeConfig(const Scenario& scenario) {
    RelevancyConfig config;
    config.max_connections = scenario.connections;
    for (uint32_t t = 0; t < 4; ++t) {
        config.types[t] = TypeRelevancy{RelevancyMode::Spatial, kRanges[t], kMargin};
    }
    return config;
}

struct Measurement {
    double mean_update_ms = 0.0;
    double full_evaluations_per_tick = 0.0; // connections evaluated in full per tick, on average
    double average_visible = 0.0;
    double bytes_per_connection = 0.0;
};

Measurement Measure(World& world, SpatialGrid& grid, uint32_t workers) {
    const Scenario& scenario = world.scenario;
    RelevancyGroups groups;
    ThreadPoolJobSystem jobs(workers);
    RelevancyEngine engine(MakeConfig(scenario), grid, groups, jobs);
    std::vector<Observer> eyes = world.PlaceObservers();

    const size_t before = ResidentBytes();
    for (uint32_t c = 0; c < scenario.connections; ++c) {
        engine.AddConnection(c);
        engine.SetObservers(c, std::span<const Observer>(&eyes[c], 1));
    }
    uint64_t tick = 0;
    const auto clear_events = [&] {
        for (uint32_t c = 0; c < scenario.connections; ++c) {
            engine.ClearEvents(c);
        }
    };
    for (int i = 0; i < kWarmupTicks; ++i, ++tick) {
        world.MoveSome(grid);
        engine.Update(tick);
        clear_events();
    }
    const size_t after = ResidentBytes();

    std::vector<double> run_means;
    uint64_t full_evaluations = 0;
    for (int run = 0; run < kRuns; ++run) {
        double total = 0.0;
        for (int i = 0; i < kTicksPerRun; ++i, ++tick) {
            world.MoveSome(grid);
            world.StepObservers(eyes);
            for (uint32_t c = 0; c < scenario.connections; ++c) {
                engine.SetObservers(c, std::span<const Observer>(&eyes[c], 1));
            }
            uint32_t evaluated = 0;
            const auto start = Clock::now();
            engine.Update(tick, &evaluated);
            total += Ms(start, Clock::now());
            full_evaluations += evaluated;
            clear_events();
        }
        run_means.push_back(total / kTicksPerRun);
    }

    Measurement result;
    result.mean_update_ms = Median(run_means);
    result.full_evaluations_per_tick = static_cast<double>(full_evaluations) / (kRuns * kTicksPerRun);
    double visible = 0.0;
    for (uint32_t c = 0; c < scenario.connections; ++c) {
        visible += engine.VisibleCount(c);
    }
    result.average_visible = visible / scenario.connections;
    result.bytes_per_connection = static_cast<double>(after - before) / scenario.connections;
    return result;
}

/// Runs 30 ticks on a world of its own, so the engine has the grid to itself, and returns a hash of every
/// event of every connection, in order. Both runs build the same world from the same seed.
uint64_t EventHash(uint32_t workers) {
    constexpr uint64_t kFnvPrime = 1099511628211ull;
    SpatialGrid grid(GridConfig{128.0f, 128.0f, 128.0f});
    World world(kMain);
    world.Build(grid);
    RelevancyGroups groups;
    ThreadPoolJobSystem jobs(workers);
    RelevancyEngine engine(MakeConfig(world.scenario), grid, groups, jobs);
    std::vector<Observer> eyes = world.PlaceObservers();
    for (uint32_t c = 0; c < world.scenario.connections; ++c) {
        engine.AddConnection(c);
        engine.SetObservers(c, std::span<const Observer>(&eyes[c], 1));
    }
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&](uint64_t value) { hash = (hash ^ value) * kFnvPrime; };
    for (uint64_t tick = 0; tick < 30; ++tick) {
        world.MoveSome(grid);
        world.StepObservers(eyes);
        for (uint32_t c = 0; c < world.scenario.connections; ++c) {
            engine.SetObservers(c, std::span<const Observer>(&eyes[c], 1));
        }
        engine.Update(tick);
        for (uint32_t c = 0; c < world.scenario.connections; ++c) {
            for (const VisibilityEvent& event : engine.Events(c).events) {
                mix(static_cast<uint64_t>(event.kind));
                mix(event.id.raw());
                mix(event.compact_id);
            }
            mix(c);
            engine.ClearEvents(c);
        }
    }
    return hash;
}

bool EventsMatchAcrossWorkerCounts() {
    return EventHash(1) == EventHash(8);
}

// Information only: how many entities one observer query returns and what each costs, to show where
// the update time goes.
void PrintGridQueryCost(World& world, const SpatialGrid& grid) {
    constexpr int kQueries = 300;
    const float radius = kRanges[3] + kMargin;
    const float edge = world.scenario.world_size / 10.0f;
    uint64_t candidates = 0;
    const auto start = Clock::now();
    for (int q = 0; q < kQueries; ++q) {
        const float cx = world.Random(edge, world.scenario.world_size - edge);
        const float cz = world.Random(edge, world.scenario.world_size - edge);
        grid.ForEachInSphere(cx, 25.0f, cz, radius, [&](EntityId, float, float, float) { ++candidates; });
    }
    const double total_ms = Ms(start, Clock::now());
    std::printf("grid query at the widest reach (%.0f m): %.0f candidates, %.0f us per query, %.0f ns per "
                "candidate\n",
                radius, static_cast<double>(candidates) / kQueries, total_ms * 1000.0 / kQueries,
                total_ms * 1e6 / static_cast<double>(candidates));
}

// Information only: what one re-anchor costs when it is done as the incremental engine will do it. One
// query per type at that type's own reach, the ids sorted, and the difference with what the player
// already sees taken in both directions. The observer moves 4 m between measurements, like a tolerance.
void PrintReanchorCost(World& world, const SpatialGrid& grid) {
    constexpr int kSamples = 200;
    constexpr float kStep = 4.0f;
    std::vector<uint64_t> visible;
    std::vector<uint64_t> wanted;
    std::vector<uint64_t> entering;
    std::vector<uint64_t> leaving;

    const auto collect = [&](float cx, float cz) {
        wanted.clear();
        for (uint32_t t = 0; t < 4; ++t) {
            const double range = kRanges[t];
            const double reach = range + kMargin;
            grid.ForEachInSphereOfType(t, cx, 25.0f, cz, static_cast<float>(reach),
                                       [&](EntityId id, float x, float y, float z) {
                                           const double dx = static_cast<double>(x) - cx;
                                           const double dy = static_cast<double>(y) - 25.0;
                                           const double dz = static_cast<double>(z) - cz;
                                           const double d2 = dx * dx + dy * dy + dz * dz;
                                           if (d2 <= range * range ||
                                               (d2 <= reach * reach &&
                                                std::binary_search(visible.begin(), visible.end(), id.raw()))) {
                                               wanted.push_back(id.raw());
                                           }
                                       });
        }
        std::sort(wanted.begin(), wanted.end());
        wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    };

    const float edge = world.scenario.world_size / 10.0f;
    std::vector<double> micros;
    for (int s = 0; s < kSamples; ++s) {
        float cx = world.Random(edge, world.scenario.world_size - edge);
        float cz = world.Random(edge, world.scenario.world_size - edge);
        visible.clear();
        collect(cx, cz); // the set the player already sees (untimed)
        visible = wanted;
        const float angle = world.Random(0.0f, 2.0f * kPi);
        cx += std::cos(angle) * kStep;
        cz += std::sin(angle) * kStep;

        const auto start = Clock::now();
        collect(cx, cz);
        entering.clear();
        leaving.clear();
        std::set_difference(wanted.begin(), wanted.end(), visible.begin(), visible.end(),
                            std::back_inserter(entering));
        std::set_difference(visible.begin(), visible.end(), wanted.begin(), wanted.end(),
                            std::back_inserter(leaving));
        micros.push_back(Ms(start, Clock::now()) * 1000.0);
    }
    Info("relevancy re-anchor cost (query by type + sort + merge)", Median(micros), "us");
}

void RunMainScenario() {
    SpatialGrid grid(GridConfig{128.0f, 128.0f, 128.0f});
    World world(kMain);
    const auto setup_start = Clock::now();
    world.Build(grid);
    std::printf("main scenario: %u connections, %u entities, %u moving per tick, cells 128 m (world built in "
                "%.0f ms)\n",
                kMain.connections, kMain.entities, kMain.moving_per_tick, Ms(setup_start, Clock::now()));

    PrintGridQueryCost(world, grid);
    PrintReanchorCost(world, grid);
    const Measurement single = Measure(world, grid, 1);
    const Measurement parallel = Measure(world, grid, 8);
    Report("relevancy visible per connection (low bound)", single.average_visible, "ents", 1000.0, true);
    Report("relevancy visible per connection (high bound)", single.average_visible, "ents", 2000.0);
    Info("relevancy full evaluations per tick (mean)", single.full_evaluations_per_tick, "conn");
    Report("relevancy update, 1 thread, mean per tick", single.mean_update_ms, "ms", kLimitOneThreadMs);
    Report("relevancy update, 8 workers, mean per tick", parallel.mean_update_ms, "ms", kLimitEightWorkersMs);
    Report("relevancy memory per connection (resident)", single.bytes_per_connection / (1024.0 * 1024.0),
           "MB", 2.0);
    ReportBool("relevancy events, 1 vs 8 workers, 30 ticks", EventsMatchAcrossWorkerCounts());

    // For information: the old movement, with jumps of tens of metres per tick.
    world.stress_movement = true;
    const Measurement stress_single = Measure(world, grid, 1);
    const Measurement stress_parallel = Measure(world, grid, 8);
    world.stress_movement = false;
    Info("relevancy update, 1 thread, stress movement", stress_single.mean_update_ms, "ms");
    Info("relevancy update, 8 workers, stress movement", stress_parallel.mean_update_ms, "ms");

    // For information: half of the observers drive at vehicle speed instead of one in ten.
    world.fast_fraction = 0.5f;
    const Measurement fast_single = Measure(world, grid, 1);
    const Measurement fast_parallel = Measure(world, grid, 8);
    world.fast_fraction = 0.1f;
    Info("relevancy update, 1 thread, half of the observers driving", fast_single.mean_update_ms, "ms");
    Info("relevancy update, 8 workers, half of the observers driving", fast_parallel.mean_update_ms, "ms");
}

void RunSmallScenario() {
    SpatialGrid grid(GridConfig{128.0f, 128.0f, 128.0f});
    World world(kSmall);
    world.Build(grid);
    std::printf("\nsmaller scenario: %u connections, %u entities, %u moving per tick (same entity density)\n",
                kSmall.connections, kSmall.entities, kSmall.moving_per_tick);
    const Measurement single = Measure(world, grid, 1);
    const Measurement parallel = Measure(world, grid, 8);
    Info("relevancy visible per connection", single.average_visible, "ents");
    Info("relevancy update, 1 thread, mean per tick", single.mean_update_ms, "ms");
    Info("relevancy update, 8 workers, mean per tick", parallel.mean_update_ms, "ms");
}

} // namespace

void CheckRelevancy() {
    std::printf("\nRelevancy engine\n");
    RunMainScenario();
    RunSmallScenario();
}
