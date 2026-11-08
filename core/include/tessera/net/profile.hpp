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
    // Inside the relevancy stage. The first three run on the main thread and are nested in Relevancy; the rest run
    // once per connection, on the workers.
    RelFold,       // folding the changes the grid recorded into one record per entity
    RelIndex,      // filing those changes by grid cell
    RelRegion,     // the parallel region that looks at every connection, wall time
    RelDestroy,    // per connection: reporting destroyed entities
    RelFull,       // per connection: a full look around the observers (only for the connections chosen)
    RelGather,     // per connection: gathering the changes filed near the observers
    RelSortUnique, // per connection: ordering the gathered changes and dropping repeats
    RelFilter,     // per connection: testing the distance of each gathered change
    RelFinish,     // per connection: turning the decisions into events and into the visible set
    Count
};

/// Amounts that are counted, not timed.
enum class Counter : uint8_t {
    Changes,    // entities whose position changed in the tick (after folding)
    Candidates, // changes gathered near the observers of a connection, repeats removed, added up over connections
    Near,       // of those, the ones within reach of an observer
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
constexpr size_t kCounters = static_cast<size_t>(Counter::Count);
inline std::array<std::atomic<uint64_t>, kCounters> g_counters{};

} // namespace detail

/// Starts every counter again from zero.
inline void Reset() noexcept {
    for (size_t i = 0; i < detail::kStages; ++i) {
        detail::g_nanoseconds[i].store(0, std::memory_order_relaxed);
        detail::g_calls[i].store(0, std::memory_order_relaxed);
    }
    for (size_t i = 0; i < detail::kCounters; ++i) {
        detail::g_counters[i].store(0, std::memory_order_relaxed);
    }
}

/// Adds to a counter. Call it once per connection or per tick, never once per entity.
inline void Add(Counter counter, uint64_t amount) noexcept {
    detail::g_counters[static_cast<size_t>(counter)].fetch_add(amount, std::memory_order_relaxed);
}

inline uint64_t Value(Counter counter) noexcept {
    return detail::g_counters[static_cast<size_t>(counter)].load(std::memory_order_relaxed);
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
    case Stage::RelFold:
        return "relevancy: fold changes";
    case Stage::RelIndex:
        return "relevancy: change index";
    case Stage::RelRegion:
        return "relevancy: per-connection region";
    case Stage::RelDestroy:
        return "relevancy: destructions";
    case Stage::RelFull:
        return "relevancy: full look";
    case Stage::RelGather:
        return "relevancy: gather candidates";
    case Stage::RelSortUnique:
        return "relevancy: sort candidates";
    case Stage::RelFilter:
        return "relevancy: filter candidates";
    case Stage::RelFinish:
        return "relevancy: finish";
    case Stage::Count:
        break;
    }
    return "?";
}

/// True for the stages that run on the workers, one scope per connection: their time is the sum over the workers
/// (CPU time), not the time that passed.
inline bool PerConnection(Stage stage) noexcept {
    return stage == Stage::Scan || stage == Stage::Select || stage == Stage::Write ||
           stage == Stage::RelDestroy || stage == Stage::RelFull || stage == Stage::RelGather ||
           stage == Stage::RelSortUnique || stage == Stage::RelFilter || stage == Stage::RelFinish;
}

/// True for the stages on the main thread that lie inside another stage (confirm inside transport, the three
/// wall-time steps of the relevancy inside the relevancy stage), so that adding up the main thread does not count
/// them twice.
inline bool Nested(Stage stage) noexcept {
    return stage == Stage::Confirm || stage == Stage::RelFold || stage == Stage::RelIndex ||
           stage == Stage::RelRegion;
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

#define TESSERA_PROFILE_COUNT(counter, amount) \
    ::tessera::net::profile::Add(::tessera::net::profile::Counter::counter, (amount))

#else

#define TESSERA_PROFILE_SCOPE(stage) ((void)0)
#define TESSERA_PROFILE_COUNT(counter, amount) ((void)0)

#endif
