// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/profile.hpp"
#include "test_support.hpp"

#if TESSERA_PROFILE
#include "network_world.hpp"
#include "tessera/net/job_system.hpp"

#include <chrono>
#include <cstdint>

using namespace tessera::net;

namespace {

void SpinFor(std::chrono::microseconds duration) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < duration) {
    }
}

} // namespace

TESSERA_TEST(a_profile_scope_adds_its_time_and_one_call) {
    profile::Reset();
    {
        TESSERA_PROFILE_SCOPE(Transport);
        SpinFor(std::chrono::microseconds(200));
    }
    TESSERA_CHECK(profile::Totals(profile::Stage::Transport).calls == 1);
    TESSERA_CHECK(profile::Totals(profile::Stage::Transport).nanoseconds >= 200000);
    TESSERA_CHECK(profile::Totals(profile::Stage::Scan).calls == 0);
}

TESSERA_TEST(profile_reset_clears_every_stage) {
    for (uint8_t i = 0; i < static_cast<uint8_t>(profile::Stage::Count); ++i) {
        profile::ScopedTimer timer(static_cast<profile::Stage>(i));
    }
    profile::Reset();
    for (uint8_t i = 0; i < static_cast<uint8_t>(profile::Stage::Count); ++i) {
        const profile::StageTotals totals = profile::Totals(static_cast<profile::Stage>(i));
        TESSERA_CHECK(totals.calls == 0 && totals.nanoseconds == 0);
    }
}

TESSERA_TEST(profile_scopes_from_many_threads_are_all_counted) {
    profile::Reset();
    ThreadPoolJobSystem jobs(4);
    jobs.ParallelFor(64, 1, [](uint32_t begin, uint32_t end, uint32_t) {
        for (uint32_t i = begin; i < end; ++i) {
            TESSERA_PROFILE_SCOPE(Scan);
        }
    });
    TESSERA_CHECK(profile::Totals(profile::Stage::Scan).calls == 64);
}

TESSERA_TEST(the_profile_timer_cost_is_small_and_reported) {
    const double cost = profile::TimerCostNanoseconds();
    TESSERA_CHECK(cost > 0.0);
    TESSERA_CHECK(cost < 5000.0);
}

TESSERA_TEST(a_server_tick_times_every_stage) {
    network_test::NetParams params;
    params.clients = 4;
    params.rate_kbps = 20; // the budget is on, so entities are selected before they are written
    network_test::NetWorld world(params);
    world.Play(20); // everyone connects and the first state goes out
    profile::Reset();
    world.Play(20);

    using profile::Stage;
    using profile::Totals;
    // Once per server tick, whatever the number of connections.
    TESSERA_CHECK(Totals(Stage::Transport).calls == 20);
    TESSERA_CHECK(Totals(Stage::Relevancy).calls == 20);
    TESSERA_CHECK(Totals(Stage::EventBatches).calls == 20);
    TESSERA_CHECK(Totals(Stage::Mark).calls == 20);
    TESSERA_CHECK(Totals(Stage::EncodeWall).calls == 20);
    TESSERA_CHECK(Totals(Stage::SendBuffers).calls == 20);
    // Once per connection and tick, in the three parts of the encoding.
    TESSERA_CHECK(Totals(Stage::Scan).calls == 4 * 20);
    TESSERA_CHECK(Totals(Stage::Select).calls == Totals(Stage::Scan).calls);
    TESSERA_CHECK(Totals(Stage::Write).calls == Totals(Stage::Scan).calls);
    // The players confirm what they receive, on the main thread and inside the transport stage.
    TESSERA_CHECK(Totals(Stage::Confirm).calls > 0);
    TESSERA_CHECK(Totals(Stage::Transport).nanoseconds >= Totals(Stage::Confirm).nanoseconds);
}

TESSERA_TEST(a_server_tick_times_the_relevancy_stages) {
    network_test::NetParams params;
    params.clients = 4;
    params.rate_kbps = 20;
    network_test::NetWorld world(params);
    world.Play(20);
    profile::Reset();
    world.Play(20);

    using profile::Counter;
    using profile::Stage;
    using profile::Totals;
    // Once per tick: the two serial steps and the parallel region.
    TESSERA_CHECK(Totals(Stage::RelFold).calls == 20);
    TESSERA_CHECK(Totals(Stage::RelIndex).calls == 20);
    TESSERA_CHECK(Totals(Stage::RelRegion).calls == 20);
    // Once per connection and tick, inside the region.
    TESSERA_CHECK(Totals(Stage::RelDestroy).calls == 4 * 20);
    TESSERA_CHECK(Totals(Stage::RelFinish).calls == 4 * 20);
    // The three parts of looking at the changes run together, and only for connections that have anchors and
    // something changed.
    const uint64_t looks = Totals(Stage::RelGather).calls;
    TESSERA_CHECK(looks > 0 && looks <= 4 * 20);
    TESSERA_CHECK(Totals(Stage::RelSortUnique).calls == looks);
    TESSERA_CHECK(Totals(Stage::RelFilter).calls == looks);
    // What was counted: changes, the candidates gathered, and those that turned out to be near.
    TESSERA_CHECK(profile::Value(Counter::Changes) > 0);
    TESSERA_CHECK(profile::Value(Counter::Near) > 0);
    TESSERA_CHECK(profile::Value(Counter::Candidates) >= profile::Value(Counter::Near));
    // The new stages that sit inside the relevancy stage fit in it, and are the ones marked as nested.
    TESSERA_CHECK(profile::Nested(Stage::RelFold) && profile::Nested(Stage::RelIndex) &&
                  profile::Nested(Stage::RelRegion) && !profile::Nested(Stage::RelGather));
    TESSERA_CHECK(Totals(Stage::Relevancy).nanoseconds >= Totals(Stage::RelFold).nanoseconds +
                                                              Totals(Stage::RelIndex).nanoseconds +
                                                              Totals(Stage::RelRegion).nanoseconds);
}

TESSERA_TEST(the_counters_are_cleared_by_reset) {
    profile::Add(profile::Counter::Changes, 5);
    TESSERA_CHECK(profile::Value(profile::Counter::Changes) >= 5);
    profile::Reset();
    TESSERA_CHECK(profile::Value(profile::Counter::Changes) == 0);
}

#else

TESSERA_TEST(the_profile_macro_is_empty_when_off) {
    TESSERA_PROFILE_SCOPE(Transport); // expands to nothing: the stage name is never looked at
    static_assert(TESSERA_PROFILE == 0);
}

#endif
