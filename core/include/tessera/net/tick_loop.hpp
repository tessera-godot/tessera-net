// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "tessera/net/job_system.hpp"

namespace tessera::net {

/// The stages of one tick, in the order they run.
enum class Phase : uint8_t {
    Input,      ///< Apply player input.
    Simulate,   ///< Run the simulation, in parallel.
    Structure,  ///< Apply the creations and destructions queued during Simulate.
    Grid,       ///< Update the spatial grid.
    Network,    ///< Decide relevancy and replicate.
};

inline constexpr size_t kPhaseCount = 5;

/// What a phase handler receives.
struct PhaseContext {
    uint64_t tick;     ///< Index of the tick, starting at 0.
    double dt_seconds; ///< Length of one tick (fixed).
    IJobSystem& jobs;  ///< Use it to spread the work over the cores.
};

struct TickConfig {
    double tick_rate_hz = 30.0;
};

/// Timing statistics. Times are in milliseconds.
struct TickMetrics {
    uint64_t ticks = 0;
    double mean_ms = 0.0;
    double p99_ms = 0.0;  ///< 99th percentile over the most recent ticks.
    double max_ms = 0.0;
    std::array<double, kPhaseCount> phase_mean_ms{};
    uint64_t overruns = 0;  ///< Ticks that took longer than the budget.
};

/// Receives the result of every tick so it can react to overload (for example by sending fewer
/// updates for low-priority entities).
class ILoadPolicy {
public:
    virtual ~ILoadPolicy() = default;
    virtual void OnTickCompleted(uint64_t tick, double elapsed_ms, double budget_ms) = 0;
};

/// Runs the phases at a fixed rate. The step never changes: if a tick takes too long, the next one
/// starts immediately and the schedule restarts from there. Ticks are never run back to back to
/// "catch up".
class TickLoop {
public:
    /// Throws std::invalid_argument if the rate is not a positive, finite number.
    TickLoop(TickConfig config, IJobSystem& jobs, ILoadPolicy* policy = nullptr);

    void SetPhaseHandler(Phase phase, std::function<void(PhaseContext&)> handler);

    /// Runs the five phases once, in order, and updates the metrics. Does not wait.
    void RunTick();

    /// Runs ticks in real time until `stop` becomes true (checked after every tick).
    void Run(const std::atomic<bool>& stop);

    const TickMetrics& Metrics() const noexcept { return metrics_; }

    /// Time available per tick, in milliseconds.
    double BudgetMs() const noexcept { return budget_ms_; }

private:
    static constexpr size_t kWindow = 1024;

    void RecordTick(double elapsed_ms, const std::array<double, kPhaseCount>& phase_ms);
    static void WaitUntil(std::chrono::steady_clock::time_point deadline);

    double dt_seconds_ = 0.0;
    double budget_ms_;
    IJobSystem& jobs_;
    ILoadPolicy* policy_;
    std::array<std::function<void(PhaseContext&)>, kPhaseCount> handlers_;
    TickMetrics metrics_;
    uint64_t next_tick_ = 0;
    double total_ms_sum_ = 0.0;
    std::array<double, kPhaseCount> phase_ms_sum_{};
    std::array<double, kWindow> recent_ms_{};
    std::array<double, kWindow> scratch_ms_{};
    size_t recent_count_ = 0;
    size_t recent_next_ = 0;
};

}  // namespace tessera::net
