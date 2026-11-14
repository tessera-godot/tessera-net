// SPDX-License-Identifier: Apache-2.0
// The interpolator's display clock: where it starts, how it advances, how it keeps its distance from the
// newest state and what it does when states stop coming or come in a rush.
#include "interpolation_test_support.hpp"
#include "tessera/net/interpolator.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace tessera::net;
using namespace interpolation_test;

namespace {

struct Clock {
    ReplicationSchema schema = MakeTestSchema();
    Interpolator interpolator;

    explicit Clock(uint32_t delay = 2) : interpolator(schema) {
        InterpolationConfig config;
        config.delay_ticks = delay;
        interpolator.Configure(config, nullptr);
        interpolator.Reset(8);
    }

    double Target(uint64_t latest) const {
        const uint64_t delay = interpolator.Config().delay_ticks;
        return latest > delay ? static_cast<double>(latest - delay) : 0.0;
    }
};

bool Near(double a, double b, double tolerance = 1e-9) {
    return std::fabs(a - b) <= tolerance;
}

} // namespace

TESSERA_TEST(before_the_first_buffer_there_is_no_display_time) {
    Clock c;
    TESSERA_CHECK(!c.interpolator.Running());
    c.interpolator.Advance(5.0);
    TESSERA_CHECK(!c.interpolator.Running());
    TESSERA_CHECK(c.interpolator.DisplayTick() == 0.0);
    TESSERA_CHECK(c.interpolator.LatestTick() == 0);
    TESSERA_CHECK(c.interpolator.LagTicks() == 0.0);

    // Interpolation that is off has no clock either.
    const ReplicationSchema schema = MakeTestSchema();
    Interpolator off(schema);
    off.OnBuffer(10);
    off.Advance(1.0);
    TESSERA_CHECK(!off.Running());
}

TESSERA_TEST(the_first_buffer_sets_the_display_time_to_its_tick_minus_the_delay) {
    Clock c(2);
    c.interpolator.OnBuffer(100);
    TESSERA_CHECK(c.interpolator.Running());
    TESSERA_CHECK(c.interpolator.DisplayTick() == 98.0);
    TESSERA_CHECK(c.interpolator.LatestTick() == 100);
    TESSERA_CHECK(Near(c.interpolator.LagTicks(), 2.0));

    Clock early(2);
    early.interpolator.OnBuffer(1);
    TESSERA_CHECK(early.interpolator.DisplayTick() == 0.0); // never before the start
    Clock zero(2);
    zero.interpolator.OnBuffer(0);
    TESSERA_CHECK(zero.interpolator.Running() && zero.interpolator.DisplayTick() == 0.0);
}

TESSERA_TEST(display_time_advances_with_the_time_given) {
    Clock c(2);
    c.interpolator.OnBuffer(100);
    c.interpolator.Advance(1.0);
    TESSERA_CHECK(Near(c.interpolator.DisplayTick(), 99.0));
    c.interpolator.Advance(0.5);
    TESSERA_CHECK(Near(c.interpolator.DisplayTick(), 99.5));
    TESSERA_CHECK(Near(c.interpolator.LagTicks(), 0.5));
}

TESSERA_TEST(advance_ignores_negative_and_not_finite_time) {
    Clock c(2);
    c.interpolator.OnBuffer(100);
    c.interpolator.Advance(0.5);
    const double before = c.interpolator.DisplayTick();
    for (double bad : {-1.0, 0.0, -0.0, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        c.interpolator.Advance(bad);
        TESSERA_CHECK(c.interpolator.DisplayTick() == before);
    }
    TESSERA_CHECK(c.interpolator.Stats().stalls == 0);
}

TESSERA_TEST(with_a_steady_stream_the_display_time_stays_delay_behind_without_corrections) {
    for (const int frames_per_tick : {1, 2}) {
        Clock c(2);
        for (uint64_t tick = 100; tick < 400; ++tick) {
            c.interpolator.OnBuffer(tick);
            const double target = c.Target(tick);
            const double after_buffer = c.interpolator.DisplayTick() - target;
            TESSERA_CHECK(after_buffer >= -1e-9 && after_buffer <= 1.0 + 1e-9);
            for (int frame = 0; frame < frames_per_tick; ++frame) {
                c.interpolator.Advance(1.0 / frames_per_tick);
            }
            const double after_advance = c.interpolator.DisplayTick() - target;
            TESSERA_CHECK(after_advance >= -1e-9 && after_advance <= 1.0 + 1e-9);
            // Not corrected at all: the shown time is exactly as far from the target as the time given.
            TESSERA_CHECK(Near(after_buffer, 0.0) && Near(after_advance, 1.0));
        }
        TESSERA_CHECK(c.interpolator.Stats().resyncs == 0);
        TESSERA_CHECK(c.interpolator.Stats().stalls == 0);
    }
}

TESSERA_TEST(display_time_never_passes_the_newest_tick_and_counts_the_stall) {
    Clock c(2);
    c.interpolator.OnBuffer(100);
    for (int step = 0; step < 30; ++step) {
        c.interpolator.Advance(1.0);
        TESSERA_CHECK(c.interpolator.DisplayTick() <= 100.0);
    }
    TESSERA_CHECK(c.interpolator.DisplayTick() == 100.0);
    TESSERA_CHECK(c.interpolator.DisplayTick() == static_cast<double>(c.interpolator.LatestTick()));
    TESSERA_CHECK(c.interpolator.Stats().stalls > 0);
    TESSERA_CHECK(c.interpolator.LagTicks() == 0.0);
}

TESSERA_TEST(a_late_buffer_within_the_delay_does_not_stop_the_display) {
    Clock c(2);
    double previous = 0.0;
    for (uint64_t step = 100; step < 200; ++step) {
        if (step == 150) {
            // The buffer of this tick is on its way; the next ones are not.
        } else if (step == 152) {
            c.interpolator.OnBuffer(152);
            c.interpolator.OnBuffer(150); // two ticks late
        } else {
            c.interpolator.OnBuffer(step);
        }
        c.interpolator.Advance(1.0);
        if (step > 100) {
            TESSERA_CHECK(c.interpolator.DisplayTick() > previous);
        }
        previous = c.interpolator.DisplayTick();
    }
    TESSERA_CHECK(c.interpolator.Stats().stalls == 0);
    TESSERA_CHECK(c.interpolator.Stats().resyncs == 0);
    TESSERA_CHECK(c.interpolator.LatestTick() == 199);
}

TESSERA_TEST(a_display_time_that_lags_by_more_than_the_band_catches_up_a_tenth_faster) {
    Clock c(0);
    c.interpolator.OnBuffer(100); // shown: 100
    c.interpolator.OnBuffer(104); // target 104, shown 100: 4 behind, the limit before a jump
    TESSERA_CHECK(c.interpolator.Stats().resyncs == 0);
    TESSERA_CHECK(c.interpolator.DisplayTick() == 100.0);
    double previous = c.interpolator.DisplayTick();
    std::vector<double> steps;
    for (int step = 0; step < 4; ++step) {
        c.interpolator.Advance(1.0);
        steps.push_back(c.interpolator.DisplayTick() - previous);
        previous = c.interpolator.DisplayTick();
    }
    TESSERA_CHECK(Near(steps[0], 1.1)); // 4 behind
    TESSERA_CHECK(Near(steps[1], 1.1)); // 2.9 behind
    TESSERA_CHECK(Near(steps[2], 1.0)); // 1.8 behind: inside the band again
    TESSERA_CHECK(Near(steps[3], 0.8)); // the newest tick (104) cuts it short
    TESSERA_CHECK(c.interpolator.DisplayTick() == 104.0);
}

TESSERA_TEST(a_display_time_that_leads_by_more_than_the_band_slows_down_a_tenth) {
    Clock c(6);
    c.interpolator.OnBuffer(100); // shown 94
    c.interpolator.Advance(3.0);  // 3 ahead of the target now
    TESSERA_CHECK(Near(c.interpolator.DisplayTick() - 94.0, 3.0));
    double previous = c.interpolator.DisplayTick();
    c.interpolator.Advance(1.0);
    TESSERA_CHECK(Near(c.interpolator.DisplayTick() - previous, 0.9));
    previous = c.interpolator.DisplayTick();
    c.interpolator.Advance(1.0);
    TESSERA_CHECK(Near(c.interpolator.DisplayTick() - previous, 0.9));
    // Once the target has moved on and the lead is inside the band, it runs at full speed again.
    c.interpolator.OnBuffer(103);
    previous = c.interpolator.DisplayTick();
    c.interpolator.Advance(1.0);
    TESSERA_CHECK(Near(c.interpolator.DisplayTick() - previous, 1.0));
    TESSERA_CHECK(c.interpolator.Stats().stalls == 0);
}

TESSERA_TEST(a_much_newer_tick_resyncs_the_display_time_at_once) {
    Clock far_ahead(2);
    far_ahead.interpolator.OnBuffer(100);
    far_ahead.interpolator.OnBuffer(150);
    TESSERA_CHECK(far_ahead.interpolator.DisplayTick() == 148.0);
    TESSERA_CHECK(far_ahead.interpolator.Stats().resyncs == 1);

    Clock at_the_limit(2);
    at_the_limit.interpolator.OnBuffer(100);
    at_the_limit.interpolator.OnBuffer(104); // 4 behind exactly
    TESSERA_CHECK(at_the_limit.interpolator.Stats().resyncs == 0);
    TESSERA_CHECK(at_the_limit.interpolator.DisplayTick() == 98.0);

    Clock past_the_limit(2);
    past_the_limit.interpolator.OnBuffer(100);
    past_the_limit.interpolator.OnBuffer(105); // 5 behind
    TESSERA_CHECK(past_the_limit.interpolator.Stats().resyncs == 1);
    TESSERA_CHECK(past_the_limit.interpolator.DisplayTick() == 103.0);
}

TESSERA_TEST(after_an_outage_the_display_resumes_without_replaying_the_gap) {
    Clock c(2);
    c.interpolator.OnBuffer(100);
    double highest = 0.0;
    for (int step = 0; step < 90; ++step) {
        c.interpolator.Advance(1.0);
        highest = std::max(highest, c.interpolator.DisplayTick());
    }
    TESSERA_CHECK(highest <= 100.0); // it waited at the newest state
    c.interpolator.OnBuffer(190);
    TESSERA_CHECK(c.interpolator.DisplayTick() == 188.0);
    TESSERA_CHECK(c.interpolator.Stats().resyncs == 1);
    c.interpolator.Advance(1.0);
    TESSERA_CHECK(Near(c.interpolator.DisplayTick(), 189.0));
}

TESSERA_TEST(an_older_buffer_tick_does_not_move_the_clock_back) {
    Clock c(2);
    c.interpolator.OnBuffer(100);
    c.interpolator.OnBuffer(101);
    c.interpolator.Advance(0.5);
    const double shown = c.interpolator.DisplayTick();
    c.interpolator.OnBuffer(99);
    c.interpolator.OnBuffer(101); // a repeat
    c.interpolator.OnBuffer(50);
    TESSERA_CHECK(c.interpolator.LatestTick() == 101);
    TESSERA_CHECK(c.interpolator.DisplayTick() == shown);
    TESSERA_CHECK(c.interpolator.Stats().resyncs == 0);
}

TESSERA_TEST(a_frame_that_took_a_quarter_of_a_second_cannot_pass_the_newest_state) {
    Clock c(2);
    c.interpolator.OnBuffer(100); // shown 98, two ticks from the newest
    c.interpolator.Advance(7.5);  // a quarter of a second at 30 ticks a second
    TESSERA_CHECK(c.interpolator.DisplayTick() == 100.0);
    TESSERA_CHECK(c.interpolator.Stats().stalls == 1);
}

TESSERA_TEST(delay_zero_shows_the_newest_state_time) {
    Clock c(0);
    c.interpolator.OnBuffer(100);
    TESSERA_CHECK(c.interpolator.DisplayTick() == 100.0);
    TESSERA_CHECK(c.interpolator.LagTicks() == 0.0);
    c.interpolator.Advance(1.0);
    TESSERA_CHECK(c.interpolator.DisplayTick() == 100.0); // nothing newer to show
    c.interpolator.OnBuffer(101);
    c.interpolator.Advance(1.0);
    TESSERA_CHECK(c.interpolator.DisplayTick() == 101.0);
    TESSERA_CHECK(c.interpolator.LagTicks() == 0.0);
}

TESSERA_TEST(reset_stops_the_clock_until_the_next_buffer) {
    Clock c(2);
    c.interpolator.OnBuffer(100);
    c.interpolator.Advance(1.0);
    TESSERA_CHECK(c.interpolator.Running());
    c.interpolator.Reset(8);
    TESSERA_CHECK(!c.interpolator.Running());
    TESSERA_CHECK(c.interpolator.DisplayTick() == 0.0);
    TESSERA_CHECK(c.interpolator.LatestTick() == 0);
    c.interpolator.Advance(1.0);
    TESSERA_CHECK(c.interpolator.DisplayTick() == 0.0);
    c.interpolator.OnBuffer(40);
    TESSERA_CHECK(c.interpolator.Running() && c.interpolator.DisplayTick() == 38.0);
}
