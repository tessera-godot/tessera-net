// SPDX-License-Identifier: Apache-2.0
// Acceptance benchmark for the data core. Prints every criterion with the measured value and exits
// with code 1 if any criterion fails. Build with -DTESSERA_BUILD_BENCH=ON and run in Release.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

#include "bench_relevancy.hpp"
#include "bench_replication.hpp"
#include "bench_report.hpp"
#include "simulation_harness.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/tick_loop.hpp"

using namespace tessera::net;
using Clock = std::chrono::steady_clock;

namespace {

constexpr uint32_t kEntities = 1'000'000;
constexpr int kRuns = 5;

struct Columns {
    float px[kPageSlots], py[kPageSlots], pz[kPageSlots];
    float vx[kPageSlots], vy[kPageSlots], vz[kPageSlots];
    float health[kPageSlots];
};
using Table = EntityTable<Columns>;

using bench::Median;
using bench::Ms;
using bench::Report;
using bench::ReportBool;

std::vector<uint32_t> Permutation(uint32_t n, uint32_t seed) {
    std::vector<uint32_t> v(n);
    std::iota(v.begin(), v.end(), 0u);
    std::mt19937 rng(seed);
    std::shuffle(v.begin(), v.end(), rng);
    return v;
}

// Work that costs on the order of 100 ns per item, so the pool overhead is not what is measured.
uint64_t BusyWork(uint64_t seed) {
    uint64_t x = seed + 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 70; ++i) {
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        x *= 0x2545F4914F6CDD1Dull;
    }
    return x;
}

double TimeBusyLoop(uint32_t workers) {
    ThreadPoolJobSystem jobs(workers);
    std::vector<uint64_t> sink(workers, 0);
    std::vector<double> times;
    for (int run = 0; run < kRuns; ++run) {
        const auto a = Clock::now();
        jobs.ParallelFor(kEntities, 4096, [&](uint32_t begin, uint32_t end, uint32_t worker) {
            uint64_t local = 0;
            for (uint32_t i = begin; i < end; ++i) {
                local += BusyWork(i);
            }
            sink[worker] += local;
        });
        times.push_back(Ms(a, Clock::now()));
    }
    volatile uint64_t keep = sink[0];
    (void)keep;
    return Median(times);
}

void CheckStorage() {
    std::vector<double> create, destroy, churn, iterate;
    for (int run = 0; run < kRuns; ++run) {
        Table table(TableConfig{});
        std::vector<EntityId> ids(kEntities, EntityId::Null());
        auto a = Clock::now();
        for (EntityId& id : ids) {
            id = table.Create();
        }
        auto b = Clock::now();
        // iterate: one pass applies position += velocity * dt to every live entity
        for (int pass = 0; pass < 20; ++pass) {
            for (uint32_t page = 0; page < table.PageCount(); ++page) {
                Columns& c = table.Columns(page);
                table.ForEachAliveInPage(page, [&](uint32_t slot) {
                    c.px[slot] += c.vx[slot] * 0.033f;
                    c.py[slot] += c.vy[slot] * 0.033f;
                    c.pz[slot] += c.vz[slot] * 0.033f;
                });
            }
        }
        auto c = Clock::now();
        // churn: 10 rounds of destroying 10% at random and creating the same number again
        const uint32_t tenth = kEntities / 10;
        std::vector<std::vector<uint32_t>> picks;  // prepared outside the timed section
        for (int round = 0; round < 10; ++round) {
            picks.push_back(Permutation(kEntities, 100 + static_cast<uint32_t>(round)));
        }
        auto d = Clock::now();
        for (int round = 0; round < 10; ++round) {
            const std::vector<uint32_t>& pick = picks[static_cast<size_t>(round)];
            for (uint32_t i = 0; i < tenth; ++i) {
                table.Destroy(ids[pick[i]]);
            }
            for (uint32_t i = 0; i < tenth; ++i) {
                ids[pick[i]] = table.Create();
            }
        }
        auto e = Clock::now();
        for (const EntityId id : ids) {
            table.Destroy(id);
        }
        auto f = Clock::now();
        create.push_back(Ms(a, b));
        iterate.push_back(Ms(b, c) / 20.0);
        churn.push_back(Ms(d, e));
        destroy.push_back(Ms(e, f));
    }
    Report("create 1M entities", Median(create), "ms", 32.0);
    Report("churn (10 rounds x 10%)", Median(churn), "ms", 420.0);
    Report("iterate 1M entities (per pass)", Median(iterate), "ms", 3.0);
    constexpr double overhead = static_cast<double>(Table::PageBytes() - sizeof(Columns)) / kPageSlots;
    Report("table overhead per entity", overhead, "B", 8.0);
}

// Destroying 1M entities right after creating them, in creation order (the scenario the limit was
// derived from), and in random order (reported for information, it is dominated by cache misses).
void CheckDestroyOrders() {
    std::vector<double> in_order, random_order;
    for (int run = 0; run < kRuns; ++run) {
        for (const bool random : {false, true}) {
            Table table(TableConfig{});
            std::vector<EntityId> ids(kEntities, EntityId::Null());
            for (EntityId& id : ids) {
                id = table.Create();
            }
            if (random) {
                std::mt19937 rng(7);
                std::shuffle(ids.begin(), ids.end(), rng);
            }
            const auto a = Clock::now();
            for (const EntityId id : ids) {
                table.Destroy(id);
            }
            (random ? random_order : in_order).push_back(Ms(a, Clock::now()));
        }
    }
    Report("destroy 1M entities (creation order)", Median(in_order), "ms", 12.0);
    std::printf("%-46s %12.2f %-6s (information only)\n", "destroy 1M entities (random order)", Median(random_order), "ms");
}

void CheckScaling() {
    const double one = TimeBusyLoop(1);
    const double eight = TimeBusyLoop(8);
    const double efficiency = 100.0 * one / (8.0 * eight);
    std::printf("  (busy loop: 1 worker %.1f ms, 8 workers %.1f ms)\n", one, eight);
    Report("parallel efficiency with 8 workers", efficiency, "%", 70.0, true);
}

void CheckTickJitter() {
    ThreadPoolJobSystem jobs(1);
    TickLoop loop(TickConfig{}, jobs);
    std::atomic<bool> stop{false};
    std::vector<Clock::time_point> starts;
    starts.reserve(400);
    loop.SetPhaseHandler(Phase::Input, [&](PhaseContext& context) {
        starts.push_back(Clock::now());
        if (context.tick == 299) {
            stop.store(true);
        }
    });
    loop.Run(stop);
    std::vector<double> deviation;
    const double period_ms = loop.BudgetMs();
    for (size_t i = 1; i < starts.size(); ++i) {
        deviation.push_back(std::fabs(Ms(starts[i - 1], starts[i]) - period_ms));
    }
    std::sort(deviation.begin(), deviation.end());
    const double p99 = deviation[(deviation.size() * 99) / 100];
    Report("tick jitter p99 at 30 Hz, no load", p99, "ms", 2.0);
}

void CheckDeterminism() {
    harness::Simulation one(1);
    harness::Simulation eight(8);
    one.RunTicks(1000);
    eight.RunTicks(1000);
    ReportBool("state hash, 1 vs 8 workers, 1000 ticks", one.StateHash() == eight.StateHash());
}

}  // namespace

int main() {
    std::printf("Tessera Net data core: acceptance benchmark (%u hardware threads)\n", std::thread::hardware_concurrency());
    std::printf("%-46s %12s %-6s %-28s\n", "criterion", "measured", "", "result");
    CheckStorage();
    CheckDestroyOrders();
    CheckScaling();
    CheckTickJitter();
    CheckDeterminism();
    CheckRelevancy();
    CheckReplication();
    const int failures = bench::Failures();
    std::printf("\n%s (%d criteria failed)\n", failures == 0 ? "ALL CRITERIA PASSED" : "SOME CRITERIA FAILED", failures);
    return failures == 0 ? 0 : 1;
}
