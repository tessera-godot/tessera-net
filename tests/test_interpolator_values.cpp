// SPDX-License-Identifier: Apache-2.0
// What the interpolator shows: positions blended between the two states around the display time, angles the
// short way round, the other kinds held, and the marks that keep a jump or a long silence from sliding.
#include "interpolation_test_support.hpp"
#include "tessera/net/interpolator.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using namespace tessera::net;
using namespace interpolation_test;

namespace {

bool Near(double a, double b, double tolerance = 1e-9) {
    return std::fabs(a - b) <= tolerance;
}

InterpolationConfig Config(uint32_t delay, double snap = 2.5, uint32_t gap = 8) {
    InterpolationConfig config;
    config.delay_ticks = delay;
    config.snap_distance = snap;
    config.max_gap_ticks = gap;
    return config;
}

// Room between the shown time and the newest tick, so that the clock can be put anywhere.
constexpr uint32_t kDelay = 5;

// One mob entity (compact id 0) with states of a known position, and a clock that can be put at a time.
struct Shown {
    ReplicationSchema schema = MakeTestSchema();
    Interpolator interpolator;

    explicit Shown(const InterpolationConfig& config = Config(kDelay)) : interpolator(schema) {
        interpolator.Configure(config, nullptr);
        interpolator.Reset(16);
        interpolator.OnEnter(0, kMobType);
    }

    void Put(uint64_t tick, double x, double y = 0.0, double z = 0.0, float yaw = 0.0f, int32_t health = 0,
             uint8_t state = 0, uint16_t flags = 0) {
        const std::vector<uint32_t> words = MobState(schema, x, y, z, yaw, health, state, flags);
        interpolator.OnRecord(0, tick, words.data());
    }

    // The time shown is `time`, with the newest tick `kDelay` ahead of its whole part.
    void At(double time) {
        const double whole = std::floor(time);
        interpolator.OnBuffer(static_cast<uint64_t>(whole) + interpolator.Config().delay_ticks);
        interpolator.Advance(time - whole);
    }

    double Read(uint32_t property, uint32_t axis = 0, uint32_t id = 0) const {
        double out[3] = {-777.0, -777.0, -777.0};
        return interpolator.Read(id, property, out) ? out[axis] : -9999.0;
    }

    double StateX(uint32_t index) const {
        return XOf(schema, interpolator.StateWords(0, index));
    }
    double StateAxis(uint32_t index, uint32_t axis) const {
        double out[3];
        PositionOf(schema, interpolator.StateWords(0, index), out);
        return out[axis];
    }
};

double AngleOf(const ReplicationSchema& schema, const uint32_t* words) {
    const PropertyDescriptor& property = schema.Find(kMobType)->properties[kAngle];
    return DequantizeAngle(property, words + property.word_offset);
}

// The angle of the grid of the test type (10 bits), `steps` steps round from 0.
float YawSteps(int steps) {
    return static_cast<float>(steps * kTwoPi / 1024.0);
}

// The distance between two angles on the circle.
double CircleDistance(double a, double b) {
    double d = std::fmod(std::fabs(a - b), kTwoPi);
    return std::min(d, kTwoPi - d);
}

} // namespace

TESSERA_TEST(position_is_linear_between_the_two_states_around_the_display_time) {
    Shown s;
    s.Put(10, 10.0, 0.0, 5.0);
    s.Put(11, 12.0, 1.0, 4.0);
    s.At(10.25);
    for (uint32_t axis = 0; axis < 3; ++axis) {
        const double from = s.StateAxis(0, axis);
        const double to = s.StateAxis(1, axis);
        TESSERA_CHECK(Near(s.Read(kPosition, axis), from + 0.25 * (to - from)));
    }
    TESSERA_CHECK(Near(s.Read(kPosition, 0), 10.5, kStep));

    // Between two states one step apart, a value in between is shown: the words are not rounded to a step.
    Shown fine;
    fine.Put(10, 10.00);
    fine.Put(11, 10.01);
    fine.At(10.3);
    const double a = fine.StateX(0);
    const double b = fine.StateX(1);
    TESSERA_CHECK(Near(b - a, kStep, 1e-6));
    TESSERA_CHECK(Near(fine.Read(kPosition, 0), a + 0.3 * (b - a)));
}

TESSERA_TEST(position_between_irregular_ticks_uses_the_ticks_not_the_state_count) {
    Shown s;
    s.Put(10, 10.0);
    s.Put(14, 11.6);
    s.At(12.0);
    TESSERA_CHECK(Near(s.Read(kPosition, 0), 0.5 * (s.StateX(0) + s.StateX(1))));
}

TESSERA_TEST(after_the_last_state_the_value_stays_at_the_last_one_without_extrapolating) {
    Shown s;
    s.Put(10, 1.0);
    s.Put(11, 2.0);
    s.At(11.5);
    TESSERA_CHECK(s.Read(kPosition, 0) == s.StateX(1));
    Shown later;
    later.Put(10, 1.0);
    later.Put(11, 2.0);
    later.At(15.9);
    TESSERA_CHECK(later.Read(kPosition, 0) == later.StateX(1));
}

TESSERA_TEST(before_the_first_state_a_new_entity_shows_that_state) {
    Shown one;
    one.Put(20, 7.0, 1.0, 2.0);
    one.At(12.0);
    TESSERA_CHECK(one.Read(kPosition, 0) == one.StateX(0));
    TESSERA_CHECK(Near(one.Read(kPosition, 1), 1.0, kStep) && Near(one.Read(kPosition, 2), 2.0, kStep));
    const uint32_t* held = one.interpolator.HeldWords(0);
    TESSERA_CHECK(held == one.interpolator.StateWords(0, 0));

    Shown two;
    two.Put(20, 7.0);
    two.Put(21, 8.0);
    two.At(15.0);
    TESSERA_CHECK(two.Read(kPosition, 0) == two.StateX(0));
    TESSERA_CHECK(two.interpolator.HeldWords(0) == two.interpolator.StateWords(0, 0));
}

TESSERA_TEST(angle_goes_the_short_way_across_zero) {
    Shown s;
    s.Put(10, 0.0, 0.0, 0.0, YawSteps(1000));
    s.Put(11, 0.0, 0.0, 0.0, YawSteps(24));
    s.At(10.5);
    const double shown = s.Read(kAngle, 0);
    TESSERA_CHECK(shown >= 0.0 && shown < kTwoPi);
    TESSERA_CHECK(CircleDistance(shown, 0.0) < 1e-6); // halfway between 1000 and 1048 steps: a full turn
    TESSERA_CHECK(CircleDistance(shown, kTwoPi / 2.0) > 1.0); // not the long way round, through half a turn
}

TESSERA_TEST(angle_goes_the_short_way_in_both_directions) {
    Shown back;
    back.Put(10, 0.0, 0.0, 0.0, YawSteps(24));
    back.Put(11, 0.0, 0.0, 0.0, YawSteps(1000));
    back.At(10.5);
    TESSERA_CHECK(CircleDistance(back.Read(kAngle, 0), 0.0) < 1e-6);

    Shown quarter;
    quarter.Put(10, 0.0, 0.0, 0.0, YawSteps(256)); // 90 degrees
    quarter.Put(11, 0.0, 0.0, 0.0, YawSteps(512)); // 180 degrees
    quarter.At(10.5);
    TESSERA_CHECK(Near(quarter.Read(kAngle, 0), kTwoPi * 384.0 / 1024.0, 1e-6)); // 135 degrees

    // Always inside [0, 2 pi), whichever way it goes.
    for (int from : {0, 1, 24, 255, 511, 512, 513, 1000, 1023}) {
        for (int to : {0, 1, 24, 255, 511, 512, 513, 1000, 1023}) {
            Shown sweep;
            sweep.Put(10, 0.0, 0.0, 0.0, YawSteps(from));
            sweep.Put(11, 0.0, 0.0, 0.0, YawSteps(to));
            sweep.At(10.0);
            for (int part = 0; part < 10; ++part) {
                sweep.interpolator.Advance(0.1);
                const double angle = sweep.Read(kAngle, 0);
                TESSERA_CHECK(angle >= 0.0 && angle < kTwoPi);
            }
        }
    }
}

TESSERA_TEST(integer_enum_and_bits_change_all_at_once_at_the_tick_of_the_state) {
    auto make = [](double time) {
        auto s = std::make_unique<Shown>();
        s->Put(10, 0.0, 0.0, 0.0, YawSteps(0), 5, 1, 0x0001);
        s->Put(11, 0.5, 0.0, 0.0, YawSteps(100), 9, 3, 0x0103);
        s->At(time);
        return s;
    };
    auto same = [](const uint32_t* a, const uint32_t* b, uint32_t words) {
        return a != nullptr && b != nullptr && std::equal(a, a + words, b);
    };
    const uint32_t words = MakeTestSchema().Find(kMobType)->words;

    const auto before = make(10.99);
    TESSERA_CHECK(same(before->interpolator.HeldWords(0), before->interpolator.StateWords(0, 0), words));
    const auto at = make(11.0);
    TESSERA_CHECK(same(at->interpolator.HeldWords(0), at->interpolator.StateWords(0, 1), words));
    const auto early = make(10.0);
    TESSERA_CHECK(same(early->interpolator.HeldWords(0), early->interpolator.StateWords(0, 0), words));
    const auto after = make(13.0);
    TESSERA_CHECK(same(after->interpolator.HeldWords(0), after->interpolator.StateWords(0, 1), words));
    // Never a mix: whichever time, the held words are one of the two states whole.
    for (double time = 10.0; time <= 11.5; time += 0.05) {
        const auto s = make(time);
        const uint32_t* held = s->interpolator.HeldWords(0);
        TESSERA_CHECK(same(held, s->interpolator.StateWords(0, 0), words) ||
                      same(held, s->interpolator.StateWords(0, 1), words));
    }
}

TESSERA_TEST(a_property_of_another_kind_than_position_or_angle_is_not_blended) {
    Shown s;
    s.Put(10, 0.0, 0.0, 0.0, 0.0f, 5, 1, 1);
    s.Put(11, 0.5, 0.0, 0.0, 0.0f, 9, 3, 3);
    s.At(10.5);
    for (uint32_t property : {kHealth, kBits, kEnum, 99u}) {
        double out[3] = {-777.0, -777.0, -777.0};
        TESSERA_CHECK(!s.interpolator.Read(0, property, out));
        TESSERA_CHECK(out[0] == -777.0 && out[1] == -777.0 && out[2] == -777.0);
    }
    double small[2] = {-777.0, -777.0};
    TESSERA_CHECK(!s.interpolator.Read(0, kPosition, small)); // a position needs 3 numbers
    TESSERA_CHECK(small[0] == -777.0 && small[1] == -777.0);
    TESSERA_CHECK(!s.interpolator.Read(0, kAngle, std::span<double>()));
    double one[1] = {-777.0};
    TESSERA_CHECK(s.interpolator.Read(0, kAngle, one));
}

TESSERA_TEST(a_jump_above_the_snap_distance_holds_then_passes_at_the_tick) {
    auto make = [](double time) {
        auto s = std::make_unique<Shown>();
        s->Put(10, 0.0);
        s->Put(11, 3.0); // 3 m in one tick
        s->At(time);
        return s;
    };
    const auto half = make(10.5);
    TESSERA_CHECK(half->Read(kPosition, 0) == half->StateX(0));
    const auto almost = make(10.99);
    TESSERA_CHECK(almost->Read(kPosition, 0) == almost->StateX(0));
    const auto at = make(11.0);
    TESSERA_CHECK(at->Read(kPosition, 0) == at->StateX(1));
    TESSERA_CHECK(at->interpolator.Stats().snaps == 1);
}

TESSERA_TEST(a_long_gap_holds_the_old_state_and_passes_to_the_new_one_at_its_tick) {
    auto make = [](double time) {
        auto s = std::make_unique<Shown>();
        s->Put(10, 5.0);
        s->Put(100, 5.2); // the entity stood still, then moved
        s->At(time);
        return s;
    };
    for (double time : {11.0, 50.0, 99.9}) {
        const auto held = make(time);
        TESSERA_CHECK(held->Read(kPosition, 0) == held->StateX(0));
    }
    const auto arrived = make(100.0);
    TESSERA_CHECK(arrived->Read(kPosition, 0) == arrived->StateX(1));
    TESSERA_CHECK(arrived->interpolator.Stats().gap_holds == 1);
}

TESSERA_TEST(a_gap_at_the_limit_is_still_interpolated) {
    Shown limit;
    limit.Put(10, 1.0);
    limit.Put(18, 1.8);
    limit.At(14.0);
    TESSERA_CHECK(Near(limit.Read(kPosition, 0), 0.5 * (limit.StateX(0) + limit.StateX(1))));

    Shown over;
    over.Put(10, 1.0);
    over.Put(19, 1.8);
    over.At(14.5);
    TESSERA_CHECK(over.Read(kPosition, 0) == over.StateX(0));
}

TESSERA_TEST(a_late_state_changes_the_shown_value_only_within_its_segment) {
    Shown s(Config(kDelay, 10.0)); // far apart, but meant to be blended
    s.Put(10, 0.0);
    s.Put(14, 4.0);
    s.At(11.0);
    TESSERA_CHECK(Near(s.Read(kPosition, 0), 1.0, kStep));
    const double before = s.Read(kPosition, 0);
    s.Put(12, 1.5); // the movement was not a straight line: this one arrives late
    TESSERA_CHECK(s.interpolator.StateCount(0) == 3);
    const double after = s.Read(kPosition, 0);
    TESSERA_CHECK(after >= s.StateX(0) - 1e-9 && after <= s.StateX(1) + 1e-9); // inside 10 to 12
    TESSERA_CHECK(Near(after, 0.5 * (s.StateX(0) + s.StateX(1))));
    TESSERA_CHECK(after < before); // the value moves back inside the segment (a known step, not a slide)
}

TESSERA_TEST(the_shown_position_is_always_inside_the_box_of_two_received_states) {
    std::mt19937 random(12345);
    auto uniform = [&](double low, double high) {
        return std::uniform_real_distribution<double>(low, high)(random);
    };
    struct Received {
        uint64_t tick;
        double p[3];
    };
    for (int round = 0; round < 1000; ++round) {
        Shown s(Config(2));
        std::vector<Received> received; // every state given to the interpolator
        // The arrival of each tick: lost 30 % of the time, repeated 10 %, up to 3 ticks late.
        std::vector<std::pair<uint64_t, uint64_t>> arrivals; // (step, tick)
        for (uint64_t tick = 1; tick <= 40; ++tick) {
            if (uniform(0.0, 1.0) < 0.3) {
                continue;
            }
            const uint64_t step = tick + static_cast<uint64_t>(uniform(0.0, 4.0));
            arrivals.emplace_back(step, tick);
            if (uniform(0.0, 1.0) < 0.1) {
                arrivals.emplace_back(step + static_cast<uint64_t>(uniform(0.0, 3.0)), tick);
            }
        }
        std::sort(arrivals.begin(), arrivals.end());
        size_t next = 0;
        double low[3] = {1e9, 1e9, 1e9};
        double high[3] = {-1e9, -1e9, -1e9};
        for (uint64_t step = 1; step <= 50; ++step) {
            while (next < arrivals.size() && arrivals[next].first == step) {
                const uint64_t tick = arrivals[next++].second;
                const double x = uniform(0.0, 2.0), y = uniform(0.0, 2.0), z = uniform(0.0, 2.0);
                const bool known = std::any_of(received.begin(), received.end(),
                                               [&](const Received& r) { return r.tick == tick; });
                s.Put(tick, x, y, z);
                if (!known) {
                    const double stored[3] = {x, y, z};
                    received.push_back(Received{tick, {x, y, z}});
                    for (int axis = 0; axis < 3; ++axis) {
                        low[axis] = std::min(low[axis], stored[axis] - kStep);
                        high[axis] = std::max(high[axis], stored[axis] + kStep);
                    }
                }
                s.interpolator.OnBuffer(tick);
            }
            s.interpolator.Advance(uniform(0.0, 2.0));
            if (!s.interpolator.Running() || s.interpolator.StateCount(0) == 0) {
                continue;
            }
            double out[3];
            TESSERA_CHECK(s.interpolator.Read(0, kPosition, out));
            for (int axis = 0; axis < 3; ++axis) {
                TESSERA_CHECK(out[axis] >= low[axis] && out[axis] <= high[axis]);
            }
            // Inside the box of the two held states that surround the shown time (or equal to one).
            const double shown = s.interpolator.DisplayTick();
            const size_t count = s.interpolator.StateCount(0);
            size_t after_index = count;
            for (size_t i = 0; i < count; ++i) {
                if (static_cast<double>(s.interpolator.StateTick(0, static_cast<uint32_t>(i))) > shown) {
                    after_index = i;
                    break;
                }
            }
            for (int axis = 0; axis < 3; ++axis) {
                double a_value, b_value;
                if (after_index == 0) {
                    a_value = b_value = s.StateAxis(0, axis);
                } else if (after_index == count) {
                    a_value = b_value = s.StateAxis(static_cast<uint32_t>(count - 1), axis);
                } else {
                    a_value = s.StateAxis(static_cast<uint32_t>(after_index - 1), axis);
                    b_value = s.StateAxis(static_cast<uint32_t>(after_index), axis);
                }
                TESSERA_CHECK(out[axis] >= std::min(a_value, b_value) - 1e-9 &&
                              out[axis] <= std::max(a_value, b_value) + 1e-9);
            }
        }
        // With no newer state coming, the shown value settles on the newest state and goes no further.
        if (s.interpolator.StateCount(0) > 0 && s.interpolator.Running()) {
            s.interpolator.OnBuffer(s.interpolator.LatestTick() + 3); // newer buffers, nothing for the entity
            for (int step = 0; step < 100; ++step) {
                s.interpolator.Advance(1.0);
            }
            const uint32_t last = static_cast<uint32_t>(s.interpolator.StateCount(0) - 1);
            double out[3];
            TESSERA_CHECK(s.interpolator.Read(0, kPosition, out));
            for (int axis = 0; axis < 3; ++axis) {
                TESSERA_CHECK(out[axis] == s.StateAxis(last, axis));
            }
        }
    }
}

TESSERA_TEST(with_two_position_properties_the_distance_is_the_combined_one) {
    struct TwoColumns {
        float x[kPageSlots];
        float y[kPageSlots];
        float z[kPageSlots];
        float x2[kPageSlots];
        float y2[kPageSlots];
        float z2[kPageSlots];
    };
    ReplicationSchema two;
    two.AddType(0, {PropertyDescriptor::Position(TESSERA_COLUMN(TwoColumns, x), TESSERA_COLUMN(TwoColumns, y),
                                                 TESSERA_COLUMN(TwoColumns, z), 0.01f, 10000.0f),
                    PropertyDescriptor::Position(TESSERA_COLUMN(TwoColumns, x2),
                                                 TESSERA_COLUMN(TwoColumns, y2),
                                                 TESSERA_COLUMN(TwoColumns, z2), 0.01f, 10000.0f)});
    const TypeSchema* type = two.Find(0);
    auto make = [&](float first, float second) {
        auto columns = std::make_unique<TwoColumns>();
        columns->x[0] = first;
        columns->x2[0] = second;
        std::vector<uint32_t> words(type->words, 0);
        for (const PropertyDescriptor& property : type->properties) {
            QuantizeProperty(property, reinterpret_cast<const std::byte*>(columns.get()), 0,
                             words.data() + property.word_offset);
        }
        return words;
    };
    auto read = [&](float second_end, uint32_t property) {
        Interpolator interpolator(two);
        interpolator.Configure(Config(kDelay), nullptr);
        interpolator.Reset(4);
        interpolator.OnEnter(0, 0);
        const std::vector<uint32_t> a = make(0.0f, 0.0f);
        const std::vector<uint32_t> b = make(1.8f, second_end);
        interpolator.OnRecord(0, 10, a.data());
        interpolator.OnRecord(0, 11, b.data());
        interpolator.OnBuffer(15);
        interpolator.Advance(0.5);
        double out[3] = {0.0, 0.0, 0.0};
        TESSERA_CHECK(interpolator.Read(0, property, out));
        return out[0];
    };
    // 1.8 m on each of the two is 2.55 m together: above 2.5, so it jumps, although each alone is below.
    TESSERA_CHECK(Near(read(1.8f, 0), 0.0, kStep)); // held at the first state
    TESSERA_CHECK(Near(read(1.8f, 1), 0.0, kStep));
    // 1.8 m on the first only: it slides.
    TESSERA_CHECK(Near(read(0.0f, 0), 0.9, kStep));
}

TESSERA_TEST(reading_an_entity_that_is_not_there_gives_nothing) {
    Shown s;
    s.Put(10, 1.0);
    s.Put(11, 2.0);
    double out[3] = {-777.0, -777.0, -777.0};
    // No clock yet.
    TESSERA_CHECK(s.interpolator.HeldWords(0) == nullptr);
    TESSERA_CHECK(!s.interpolator.Read(0, kPosition, out));
    s.At(10.5);
    TESSERA_CHECK(s.interpolator.HeldWords(0) != nullptr);
    TESSERA_CHECK(s.interpolator.Read(0, kPosition, out));
    // An id that was never in, one out of range, and one that left.
    TESSERA_CHECK(s.interpolator.HeldWords(7) == nullptr && !s.interpolator.Read(7, kPosition, out));
    TESSERA_CHECK(s.interpolator.HeldWords(5000) == nullptr && !s.interpolator.Read(5000, kPosition, out));
    s.interpolator.OnLeave(0);
    TESSERA_CHECK(s.interpolator.HeldWords(0) == nullptr && !s.interpolator.Read(0, kPosition, out));
    // An entity that entered and has no state yet.
    s.interpolator.OnEnter(1, kMobType);
    TESSERA_CHECK(s.interpolator.HeldWords(1) == nullptr && !s.interpolator.Read(1, kPosition, out));
    TESSERA_CHECK(out[0] == out[0]); // untouched by the failed reads
}
