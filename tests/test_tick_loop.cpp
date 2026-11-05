// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

#include "tessera/net/job_system.hpp"
#include "tessera/net/tick_loop.hpp"
#include "test_support.hpp"

using tessera::net::ILoadPolicy;
using tessera::net::Phase;
using tessera::net::PhaseContext;
using tessera::net::ThreadPoolJobSystem;
using tessera::net::TickConfig;
using tessera::net::TickLoop;

namespace {

using Clock = std::chrono::steady_clock;

TickConfig Rate(double hz) {
    TickConfig config;
    config.tick_rate_hz = hz;
    return config;
}

class CountingPolicy final : public ILoadPolicy {
public:
    void OnTickCompleted(uint64_t tick, double, double budget_ms) override {
        ++calls;
        last_tick = tick;
        last_budget_ms = budget_ms;
    }
    int calls = 0;
    uint64_t last_tick = 0;
    double last_budget_ms = 0.0;
};

}  // namespace

TESSERA_TEST(phases_run_in_documented_order) {
    ThreadPoolJobSystem jobs(1);
    TickLoop loop(Rate(30.0), jobs);
    std::vector<Phase> order;
    for (Phase phase : {Phase::Input, Phase::Simulate, Phase::Structure, Phase::Grid, Phase::Network}) {
        loop.SetPhaseHandler(phase, [&order, phase](PhaseContext&) { order.push_back(phase); });
    }
    loop.RunTick();
    TESSERA_CHECK((order == std::vector<Phase>{Phase::Input, Phase::Simulate, Phase::Structure, Phase::Grid,
                                                Phase::Network}));
}

TESSERA_TEST(phase_without_handler_is_skipped) {
    ThreadPoolJobSystem jobs(1);
    TickLoop loop(Rate(30.0), jobs);
    int simulate_calls = 0;
    loop.SetPhaseHandler(Phase::Simulate, [&](PhaseContext&) { ++simulate_calls; });
    loop.RunTick();
    TESSERA_CHECK(simulate_calls == 1);
    TESSERA_CHECK(loop.Metrics().ticks == 1);
}

TESSERA_TEST(dt_matches_tick_rate) {
    ThreadPoolJobSystem jobs(1);
    TickLoop loop(Rate(30.0), jobs);
    double dt = 0.0;
    loop.SetPhaseHandler(Phase::Simulate, [&](PhaseContext& context) { dt = context.dt_seconds; });
    loop.RunTick();
    TESSERA_CHECK(std::fabs(dt - 1.0 / 30.0) < 1e-9);
}

TESSERA_TEST(budget_is_inverse_of_rate) {
    ThreadPoolJobSystem jobs(1);
    TickLoop loop(Rate(30.0), jobs);
    TESSERA_CHECK(std::fabs(loop.BudgetMs() - 33.333) < 0.001);
}

TESSERA_TEST(invalid_rate_throws) {
    ThreadPoolJobSystem jobs(1);
    int thrown = 0;
    for (double bad : {0.0, -5.0, std::numeric_limits<double>::quiet_NaN()}) {
        try {
            TickLoop loop(Rate(bad), jobs);
        } catch (const std::invalid_argument&) {
            ++thrown;
        }
    }
    TESSERA_CHECK(thrown == 3);
}

TESSERA_TEST(tick_index_advances_and_metrics_track_max) {
    ThreadPoolJobSystem jobs(1);
    TickLoop loop(Rate(30.0), jobs);
    std::vector<uint64_t> ticks;
    loop.SetPhaseHandler(Phase::Simulate, [&](PhaseContext& context) {
        ticks.push_back(context.tick);
        if (context.tick == 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    loop.RunTick();
    loop.RunTick();
    loop.RunTick();
    TESSERA_CHECK((ticks == std::vector<uint64_t>{0, 1, 2}));
    TESSERA_CHECK(loop.Metrics().ticks == 3);
    TESSERA_CHECK(loop.Metrics().max_ms >= 5.0);
    TESSERA_CHECK(loop.Metrics().p99_ms >= 5.0);
    TESSERA_CHECK(loop.Metrics().phase_mean_ms[static_cast<size_t>(Phase::Simulate)] > 1.0);
}

TESSERA_TEST(load_policy_receives_each_tick) {
    ThreadPoolJobSystem jobs(1);
    CountingPolicy policy;
    TickLoop loop(Rate(30.0), jobs, &policy);
    for (int i = 0; i < 4; ++i) {
        loop.RunTick();
    }
    TESSERA_CHECK(policy.calls == 4);
    TESSERA_CHECK(policy.last_tick == 3);
    TESSERA_CHECK(std::fabs(policy.last_budget_ms - 33.333) < 0.001);
}

TESSERA_TEST(run_stops_when_flag_set) {
    ThreadPoolJobSystem jobs(1);
    TickLoop loop(Rate(1000.0), jobs);
    std::atomic<bool> stop{false};
    int ticks = 0;
    loop.SetPhaseHandler(Phase::Network, [&](PhaseContext&) {
        if (++ticks == 5) {
            stop.store(true);
        }
    });
    loop.Run(stop);
    TESSERA_CHECK(ticks == 5);
    TESSERA_CHECK(loop.Metrics().ticks == 5);
}

TESSERA_TEST(overrun_does_not_burst) {
    ThreadPoolJobSystem jobs(1);
    TickLoop loop(Rate(100.0), jobs);  // 10 ms period
    std::atomic<bool> stop{false};
    std::vector<Clock::time_point> starts;
    loop.SetPhaseHandler(Phase::Input, [&](PhaseContext& context) {
        starts.push_back(Clock::now());
        if (context.tick == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));  // far over the budget
        }
        if (context.tick == 11) {
            stop.store(true);
        }
    });
    loop.Run(stop);
    TESSERA_CHECK(loop.Metrics().overruns >= 1);
    TESSERA_CHECK(starts.size() == 12);
    // After the slow tick the loop must resume its normal pace instead of running ticks back to back.
    // The loop guarantees two starts are never closer than half a period (5 ms here); a burst would
    // show gaps near zero.
    bool paced = true;
    for (size_t i = 2; i < starts.size(); ++i) {
        const double gap_ms = std::chrono::duration<double, std::milli>(starts[i] - starts[i - 1]).count();
        paced = paced && gap_ms >= 4.0;
    }
    TESSERA_CHECK(paced);
}
