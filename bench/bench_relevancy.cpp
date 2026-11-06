// SPDX-License-Identifier: Apache-2.0
// Acceptance benchmark for the relevancy engine. The main scenario has 128 connections and one million
// entities in a 10 km x 10 km world, 700 of them moving every tick, with about 1,000 to 2,000 entities
// visible to each connection. A second, smaller scenario is reported for information.
#include "bench_relevancy.hpp"

#include "bench_report.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/relevancy/relevancy_engine.hpp"
#include "tessera/net/spatial_grid.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <span>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// psapi.h must come after windows.h.
#include <psapi.h>
#endif

using namespace tessera::net;
using bench::Clock;
using bench::Info;
using bench::Median;
using bench::Ms;
using bench::Report;
using bench::ReportBool;

namespace {

// Limits for the main scenario. They guard against regressions: they sit above what this version
// measures, with room for run-to-run variation. They are not the performance goals of the relevancy
// engine (1.5 ms with one thread and 0.4 ms with eight workers), which need an incremental design.
constexpr double kLimitOneThreadMs = 25.0;
constexpr double kLimitEightWorkersMs = 5.0;

constexpr int kWarmupTicks = 9;
constexpr int kRuns = 5;
constexpr int kTicksPerRun = 30;

// Four spatial types; most entities are small things seen from close by, a few are seen from far.
constexpr float kRanges[4] = {100.0f, 250.0f, 400.0f, 600.0f};
constexpr float kMargin = 8.0f;

struct Scenario {
    uint32_t connections;
    uint32_t entities;
    float world_size; // side of the square world, in metres
    uint32_t moving_per_tick;
};

// The reference scenario and a smaller one with the same entity density (10,000 per square km).
constexpr Scenario kMain{128, 1'000'000, 10'000.0f, 700};
constexpr Scenario kSmall{32, 250'000, 5'000.0f, 175};

size_t ResidentBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
    return static_cast<size_t>(counters.WorkingSetSize);
#else
    std::ifstream statm("/proc/self/statm");
    size_t total_pages = 0;
    size_t resident_pages = 0;
    statm >> total_pages >> resident_pages;
    return resident_pages * 4096;
#endif
}

struct World {
    Scenario scenario;
    std::vector<EntityId> ids;
    std::vector<float> x, y, z;
    std::mt19937 rng{2024};

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
            x[i] = Clamp(x[i] + Random(-30.0f, 30.0f));
            z[i] = Clamp(z[i] + Random(-30.0f, 30.0f));
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
        return eyes;
    }

    void JitterObservers(std::vector<Observer>& eyes, RelevancyEngine& engine) {
        for (uint32_t c = 0; c < scenario.connections; ++c) {
            eyes[c].x = Clamp(eyes[c].x + Random(-10.0f, 10.0f));
            eyes[c].z = Clamp(eyes[c].z + Random(-10.0f, 10.0f));
            engine.SetObservers(c, std::span<const Observer>(&eyes[c], 1));
        }
    }
};

RelevancyConfig MakeConfig(const Scenario& scenario) {
    RelevancyConfig config;
    config.max_connections = scenario.connections;
    config.recalc_interval_ticks = 3;
    for (uint32_t t = 0; t < 4; ++t) {
        config.types[t] = TypeRelevancy{RelevancyMode::Spatial, kRanges[t], kMargin};
    }
    return config;
}

struct Measurement {
    double mean_update_ms = 0.0;
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
    for (int run = 0; run < kRuns; ++run) {
        double total = 0.0;
        for (int i = 0; i < kTicksPerRun; ++i, ++tick) {
            world.MoveSome(grid);
            world.JitterObservers(eyes, engine);
            const auto start = Clock::now();
            engine.Update(tick);
            total += Ms(start, Clock::now());
            clear_events();
        }
        run_means.push_back(total / kTicksPerRun);
    }

    Measurement result;
    result.mean_update_ms = Median(run_means);
    double visible = 0.0;
    for (uint32_t c = 0; c < scenario.connections; ++c) {
        visible += engine.VisibleCount(c);
    }
    result.average_visible = visible / scenario.connections;
    result.bytes_per_connection = static_cast<double>(after - before) / scenario.connections;
    return result;
}

/// Runs a one-thread and an eight-worker engine side by side on the same world and compares events.
bool EventsMatchAcrossWorkerCounts(World& world, SpatialGrid& grid) {
    const Scenario& scenario = world.scenario;
    RelevancyGroups groups_one;
    RelevancyGroups groups_eight;
    ThreadPoolJobSystem one(1);
    ThreadPoolJobSystem eight(8);
    RelevancyEngine engine_one(MakeConfig(scenario), grid, groups_one, one);
    RelevancyEngine engine_eight(MakeConfig(scenario), grid, groups_eight, eight);
    std::vector<Observer> eyes = world.PlaceObservers();
    for (uint32_t c = 0; c < scenario.connections; ++c) {
        engine_one.AddConnection(c);
        engine_eight.AddConnection(c);
        engine_one.SetObservers(c, std::span<const Observer>(&eyes[c], 1));
        engine_eight.SetObservers(c, std::span<const Observer>(&eyes[c], 1));
    }
    for (uint64_t tick = 0; tick < 30; ++tick) {
        world.MoveSome(grid);
        for (uint32_t c = 0; c < scenario.connections; ++c) {
            eyes[c].x = world.Clamp(eyes[c].x + world.Random(-10.0f, 10.0f));
            engine_one.SetObservers(c, std::span<const Observer>(&eyes[c], 1));
            engine_eight.SetObservers(c, std::span<const Observer>(&eyes[c], 1));
        }
        engine_one.Update(tick);
        engine_eight.Update(tick);
        for (uint32_t c = 0; c < scenario.connections; ++c) {
            const auto& a = engine_one.Events(c).events;
            const auto& b = engine_eight.Events(c).events;
            if (a.size() != b.size()) {
                return false;
            }
            for (size_t i = 0; i < a.size(); ++i) {
                if (a[i].kind != b[i].kind || a[i].id != b[i].id || a[i].compact_id != b[i].compact_id) {
                    return false;
                }
            }
            engine_one.ClearEvents(c);
            engine_eight.ClearEvents(c);
        }
    }
    return true;
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

void RunMainScenario() {
    SpatialGrid grid(GridConfig{128.0f, 128.0f, 128.0f});
    World world(kMain);
    const auto setup_start = Clock::now();
    world.Build(grid);
    std::printf("main scenario: %u connections, %u entities, %u moving per tick, cells 128 m (world built in "
                "%.0f ms)\n",
                kMain.connections, kMain.entities, kMain.moving_per_tick, Ms(setup_start, Clock::now()));

    PrintGridQueryCost(world, grid);
    const Measurement single = Measure(world, grid, 1);
    const Measurement parallel = Measure(world, grid, 8);
    Report("relevancy visible per connection (low bound)", single.average_visible, "ents", 1000.0, true);
    Report("relevancy visible per connection (high bound)", single.average_visible, "ents", 2000.0);
    Report("relevancy update, 1 thread, mean per tick", single.mean_update_ms, "ms", kLimitOneThreadMs);
    Report("relevancy update, 8 workers, mean per tick", parallel.mean_update_ms, "ms", kLimitEightWorkersMs);
    Report("relevancy memory per connection (resident)", single.bytes_per_connection / (1024.0 * 1024.0),
           "MB", 2.0);
    ReportBool("relevancy events, 1 vs 8 workers, 30 ticks", EventsMatchAcrossWorkerCounts(world, grid));
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
