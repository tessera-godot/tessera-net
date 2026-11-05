// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/tick_loop.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace tessera::net {

namespace {

using Clock = std::chrono::steady_clock;

double ElapsedMs(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

#ifdef _WIN32
// The default Windows sleep wakes up in steps of about 15 ms. A high-resolution waitable timer
// (Windows 10 1803 and later) wakes up within about a millisecond.
class HighResolutionTimer {
public:
    HighResolutionTimer() {
        constexpr DWORD kHighResolutionFlag = 0x00000002;  // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
        handle_ = CreateWaitableTimerExW(nullptr, nullptr, kHighResolutionFlag, TIMER_ALL_ACCESS);
    }
    ~HighResolutionTimer() {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }
    HighResolutionTimer(const HighResolutionTimer&) = delete;
    HighResolutionTimer& operator=(const HighResolutionTimer&) = delete;

    /// Sleeps until the deadline. Returns false if no high-resolution timer is available.
    bool SleepUntil(Clock::time_point deadline) {
        if (handle_ == nullptr) {
            return false;
        }
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) {
            return true;
        }
        LARGE_INTEGER due;
        due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100;  // 100 ns units, relative
        if (!SetWaitableTimer(handle_, &due, 0, nullptr, nullptr, FALSE)) {
            return false;
        }
        WaitForSingleObject(handle_, INFINITE);
        return true;
    }

private:
    HANDLE handle_ = nullptr;
};
#endif

void SleepUntil(Clock::time_point deadline) {
#ifdef _WIN32
    thread_local HighResolutionTimer timer;
    if (timer.SleepUntil(deadline)) {
        return;
    }
#endif
    std::this_thread::sleep_until(deadline);
}

}  // namespace

TickLoop::TickLoop(TickConfig config, IJobSystem& jobs, ILoadPolicy* policy)
    : budget_ms_(0.0), jobs_(jobs), policy_(policy) {
    if (!std::isfinite(config.tick_rate_hz) || config.tick_rate_hz <= 0.0) {
        throw std::invalid_argument("TickLoop: the tick rate must be a positive, finite number");
    }
    dt_seconds_ = 1.0 / config.tick_rate_hz;
    budget_ms_ = 1000.0 * dt_seconds_;
}

void TickLoop::SetPhaseHandler(Phase phase, std::function<void(PhaseContext&)> handler) {
    handlers_[static_cast<size_t>(phase)] = std::move(handler);
}

void TickLoop::RunTick() {
    PhaseContext context{next_tick_, dt_seconds_, jobs_};
    std::array<double, kPhaseCount> phase_ms{};
    const Clock::time_point tick_start = Clock::now();
    for (size_t i = 0; i < kPhaseCount; ++i) {
        if (handlers_[i]) {
            const Clock::time_point phase_start = Clock::now();
            handlers_[i](context);
            phase_ms[i] = ElapsedMs(phase_start, Clock::now());
        }
    }
    const double elapsed_ms = ElapsedMs(tick_start, Clock::now());
    RecordTick(elapsed_ms, phase_ms);
    if (policy_ != nullptr) {
        policy_->OnTickCompleted(next_tick_, elapsed_ms, budget_ms_);
    }
    ++next_tick_;
}

void TickLoop::Run(const std::atomic<bool>& stop) {
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(dt_seconds_));
    Clock::time_point scheduled = Clock::now();  // when the current tick is supposed to start
    while (!stop.load(std::memory_order_acquire)) {
        const Clock::time_point start = Clock::now();
        RunTick();
        const Clock::time_point end = Clock::now();

        Clock::time_point next = scheduled + period;
        if (end - start >= period) {
            // The tick itself overran: start the next one right away and restart the schedule.
            // Ticks are never run back to back to catch up.
            next = end;
        } else if (start - scheduled > period / 2) {
            // The thread woke up far too late: restart the schedule instead of squeezing ticks together.
            next = start + period;
        }
        // Either way two ticks never start less than half a period apart, except after an overrun.
        if (end < next) {
            WaitUntil(next);
        }
        scheduled = next;
    }
}

void TickLoop::WaitUntil(Clock::time_point deadline) {
    // Sleeping is imprecise on some systems, so sleep until just before the deadline and spin for the
    // rest.
    constexpr auto kSpinWindow = std::chrono::microseconds(1000);
    if (deadline - Clock::now() > kSpinWindow) {
        SleepUntil(deadline - kSpinWindow);
    }
    while (Clock::now() < deadline) {
        std::this_thread::yield();
    }
}

void TickLoop::RecordTick(double elapsed_ms, const std::array<double, kPhaseCount>& phase_ms) {
    ++metrics_.ticks;
    total_ms_sum_ += elapsed_ms;
    metrics_.mean_ms = total_ms_sum_ / static_cast<double>(metrics_.ticks);
    metrics_.max_ms = std::max(metrics_.max_ms, elapsed_ms);
    if (elapsed_ms > budget_ms_) {
        ++metrics_.overruns;
    }
    for (size_t i = 0; i < kPhaseCount; ++i) {
        phase_ms_sum_[i] += phase_ms[i];
        metrics_.phase_mean_ms[i] = phase_ms_sum_[i] / static_cast<double>(metrics_.ticks);
    }
    recent_ms_[recent_next_] = elapsed_ms;
    recent_next_ = (recent_next_ + 1) % kWindow;
    recent_count_ = std::min(recent_count_ + 1, kWindow);

    // 99th percentile of the recent window, computed in a preallocated buffer (no allocation per tick).
    std::copy_n(recent_ms_.begin(), recent_count_, scratch_ms_.begin());
    const size_t rank = (recent_count_ * 99) / 100;
    std::nth_element(scratch_ms_.begin(), scratch_ms_.begin() + static_cast<std::ptrdiff_t>(rank),
                     scratch_ms_.begin() + static_cast<std::ptrdiff_t>(recent_count_));
    metrics_.p99_ms = scratch_ms_[rank];
}

}  // namespace tessera::net
