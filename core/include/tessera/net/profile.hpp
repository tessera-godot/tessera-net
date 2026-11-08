// SPDX-License-Identifier: Apache-2.0
#pragma once

// Timing of the stages of the network phase, for measurement builds only.
//
// With TESSERA_PROFILE off (the default, and the build of every release) TESSERA_PROFILE_SCOPE expands to
// nothing, so the code the compiler sees is the code without any timing. With it on, each scope adds the time
// it lasted, and one call, to a counter of its stage. The scopes sit around whole stages (a tick, or one
// connection), never around single entities, so that measuring does not change much of what it measures.

#ifndef TESSERA_PROFILE
#define TESSERA_PROFILE 0
#endif

#if TESSERA_PROFILE

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace tessera::net::profile {

enum class Stage : uint8_t {
    Transport,    // reading the transport and everything the messages cause (main thread)
    Confirm,      // confirming ticks to the replication engine; inside Transport (main thread)
    Relevancy,    // the relevancy update (main thread)
    EventBatches, // sending and applying the visibility events (main thread)
    Mark,         // marking who needs which changed entity (main thread)
    EncodeWall,   // the parallel region that encodes every connection, wall time (main thread)
    Scan,         // per connection: collecting the entities that may be sent
    Select,       // per connection: choosing what fits the budget
    Write,        // per connection: writing the chosen records
    SendBuffers,  // sending the state buffers (main thread)
    Count
};

struct StageTotals {
    uint64_t nanoseconds = 0;
    uint64_t calls = 0;
};

namespace detail {

constexpr size_t kStages = static_cast<size_t>(Stage::Count);
inline std::array<std::atomic<uint64_t>, kStages> g_nanoseconds{};
inline std::array<std::atomic<uint64_t>, kStages> g_calls{};

} // namespace detail

/// Starts every counter again from zero.
inline void Reset() noexcept {
    for (size_t i = 0; i < detail::kStages; ++i) {
        detail::g_nanoseconds[i].store(0, std::memory_order_relaxed);
        detail::g_calls[i].store(0, std::memory_order_relaxed);
    }
}

inline StageTotals Totals(Stage stage) noexcept {
    const size_t i = static_cast<size_t>(stage);
    return StageTotals{detail::g_nanoseconds[i].load(std::memory_order_relaxed),
                       detail::g_calls[i].load(std::memory_order_relaxed)};
}

inline const char* Name(Stage stage) noexcept {
    switch (stage) {
    case Stage::Transport:
        return "transport";
    case Stage::Confirm:
        return "confirm";
    case Stage::Relevancy:
        return "relevancy";
    case Stage::EventBatches:
        return "event batches";
    case Stage::Mark:
        return "mark";
    case Stage::EncodeWall:
        return "encode";
    case Stage::Scan:
        return "scan";
    case Stage::Select:
        return "select";
    case Stage::Write:
        return "write";
    case Stage::SendBuffers:
        return "send buffers";
    case Stage::Count:
        break;
    }
    return "?";
}

/// True for the stages that run on the workers, one scope per connection: their time is the sum over the workers
/// (CPU time), not the time that passed.
inline bool PerConnection(Stage stage) noexcept {
    return stage == Stage::Scan || stage == Stage::Select || stage == Stage::Write;
}

/// Adds the time between its construction and its destruction to a stage.
class ScopedTimer {
public:
    explicit ScopedTimer(Stage stage) noexcept : stage_(stage), start_(std::chrono::steady_clock::now()) {}
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
    ~ScopedTimer() {
        const auto elapsed = std::chrono::steady_clock::now() - start_;
        const size_t i = static_cast<size_t>(stage_);
        detail::g_nanoseconds[i].fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
            std::memory_order_relaxed);
        detail::g_calls[i].fetch_add(1, std::memory_order_relaxed);
    }

private:
    Stage stage_;
    std::chrono::steady_clock::time_point start_;
};

/// What one empty scope costs, in nanoseconds, measured over a million of them. It runs on the last stage and puts
/// that stage's counters back, so measuring the timer leaves the numbers as they were.
inline double TimerCostNanoseconds() {
    constexpr int kScopes = 1'000'000;
    constexpr Stage kScratch = Stage::SendBuffers;
    const StageTotals saved = Totals(kScratch);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kScopes; ++i) {
        ScopedTimer timer(kScratch);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    detail::g_nanoseconds[static_cast<size_t>(kScratch)].store(saved.nanoseconds, std::memory_order_relaxed);
    detail::g_calls[static_cast<size_t>(kScratch)].store(saved.calls, std::memory_order_relaxed);
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) /
           kScopes;
}

} // namespace tessera::net::profile

#define TESSERA_PROFILE_JOIN_INNER(a, b) a##b
#define TESSERA_PROFILE_JOIN(a, b) TESSERA_PROFILE_JOIN_INNER(a, b)
#define TESSERA_PROFILE_SCOPE(stage)                                                              \
    ::tessera::net::profile::ScopedTimer TESSERA_PROFILE_JOIN(tessera_profile_scope_, __LINE__)( \
        ::tessera::net::profile::Stage::stage)

#else

#define TESSERA_PROFILE_SCOPE(stage) ((void)0)

#endif
