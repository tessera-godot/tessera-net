// SPDX-License-Identifier: Apache-2.0
// The interpolator's history of states: the configuration, the storage per entity type, the order of the
// states and the marks that say whether two neighbours are blended.
#include "interpolation_test_support.hpp"
#include "tessera/net/interpolator.hpp"
#include "test_support.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace tessera::net;
using namespace interpolation_test;

namespace {

InterpolationConfig Config(uint32_t delay = 2, double snap = 2.5, uint32_t gap = 8) {
    InterpolationConfig config;
    config.delay_ticks = delay;
    config.snap_distance = snap;
    config.max_gap_ticks = gap;
    return config;
}

// An interpolator that is on, with room for `max_visible` entities.
struct Fixture {
    ReplicationSchema schema = MakeTestSchema();
    Interpolator interpolator{schema};

    explicit Fixture(const InterpolationConfig& config = Config(), uint32_t max_visible = 64) {
        std::string error;
        interpolator.Configure(config, &error);
        interpolator.Reset(max_visible);
    }

    void Feed(uint32_t id, uint64_t tick, double x, double y = 0.0) {
        const std::vector<uint32_t> words = MobState(schema, x, y);
        interpolator.OnRecord(id, tick, words.data());
    }

    double XAt(uint32_t id, uint32_t index) const {
        const uint32_t* words = interpolator.StateWords(id, index);
        return words != nullptr ? XOf(schema, words) : std::numeric_limits<double>::quiet_NaN();
    }
};

} // namespace

TESSERA_TEST(a_config_with_a_delay_a_snap_distance_or_a_gap_out_of_range_is_refused) {
    const ReplicationSchema schema = MakeTestSchema();
    Interpolator interpolator(schema);
    std::string error;
    TESSERA_CHECK(!interpolator.Enabled());

    // Refused while off: it stays off.
    TESSERA_CHECK(!interpolator.Configure(Config(31), &error) && !error.empty());
    TESSERA_CHECK(!interpolator.Enabled());

    TESSERA_CHECK(interpolator.Configure(Config(4, 3.0, 12), &error));
    TESSERA_CHECK(interpolator.Enabled());

    // Refused while on: nothing changes.
    const double bad_snaps[] = {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::infinity()};
    for (double snap : bad_snaps) {
        error.clear();
        TESSERA_CHECK(!interpolator.Configure(Config(2, snap), &error) && !error.empty());
    }
    error.clear();
    TESSERA_CHECK(!interpolator.Configure(Config(31), &error) && !error.empty());
    error.clear();
    TESSERA_CHECK(!interpolator.Configure(Config(2, 2.5, 0), &error) && !error.empty());
    error.clear();
    TESSERA_CHECK(!interpolator.Configure(Config(2, 2.5, 256), &error) && !error.empty());
    TESSERA_CHECK(!interpolator.Configure(Config(31), nullptr));
    TESSERA_CHECK(interpolator.Enabled());
    TESSERA_CHECK(interpolator.Config().delay_ticks == 4);
    TESSERA_CHECK(interpolator.Config().snap_distance == 3.0);
    TESSERA_CHECK(interpolator.Config().max_gap_ticks == 12);

    // The limits themselves are fine.
    TESSERA_CHECK(interpolator.Configure(Config(30, 0.001, 255), &error));
    TESSERA_CHECK(interpolator.Configure(Config(0, 1000.0, 1), &error));
}

TESSERA_TEST(a_valid_config_sizes_the_history_from_the_delay) {
    const ReplicationSchema schema = MakeTestSchema();
    Interpolator interpolator(schema);
    TESSERA_CHECK(interpolator.Configure(Config(2), nullptr));
    TESSERA_CHECK(interpolator.Capacity() == 8);
    TESSERA_CHECK(interpolator.Configure(Config(0), nullptr));
    TESSERA_CHECK(interpolator.Capacity() == 6);
    TESSERA_CHECK(interpolator.Configure(Config(30), nullptr));
    TESSERA_CHECK(interpolator.Capacity() == 36);
}

TESSERA_TEST(an_entity_that_enters_has_no_state_until_its_first_record) {
    Fixture f;
    TESSERA_CHECK(f.interpolator.OnEnter(3, kMobType));
    TESSERA_CHECK(f.interpolator.StateCount(3) == 0);
    f.Feed(3, 10, 1.0);
    TESSERA_CHECK(f.interpolator.StateCount(3) == 1);
    TESSERA_CHECK(f.interpolator.Stats().states == 1);

    // What cannot enter: an id out of range, a type without schema, and interpolation that is off.
    TESSERA_CHECK(!f.interpolator.OnEnter(64, kMobType));
    TESSERA_CHECK(!f.interpolator.OnEnter(4, kUnknownType));
    const ReplicationSchema schema = MakeTestSchema();
    Interpolator off(schema);
    TESSERA_CHECK(!off.OnEnter(0, kMobType));
}

TESSERA_TEST(states_are_kept_in_tick_order_whatever_the_arrival_order) {
    Fixture f;
    f.interpolator.OnEnter(0, kMobType);
    f.Feed(0, 10, 10.0);
    f.Feed(0, 12, 12.0);
    f.Feed(0, 11, 11.0);
    f.Feed(0, 9, 9.0);
    TESSERA_CHECK(f.interpolator.StateCount(0) == 4);
    for (uint32_t i = 0; i < 4; ++i) {
        TESSERA_CHECK(f.interpolator.StateTick(0, i) == 9 + i);
        TESSERA_CHECK(std::fabs(f.XAt(0, i) - static_cast<double>(9 + i)) < kStep);
    }
    TESSERA_CHECK(f.interpolator.Stats().states == 4);
    TESSERA_CHECK(f.interpolator.Stats().states_ignored == 0);
}

TESSERA_TEST(a_repeated_tick_is_ignored_and_counted) {
    Fixture f;
    f.interpolator.OnEnter(0, kMobType);
    f.Feed(0, 10, 1.0);
    f.Feed(0, 10, 2.0);
    TESSERA_CHECK(f.interpolator.StateCount(0) == 1);
    TESSERA_CHECK(f.interpolator.Stats().states == 1);
    TESSERA_CHECK(f.interpolator.Stats().states_ignored == 1);
    TESSERA_CHECK(std::fabs(f.XAt(0, 0) - 1.0) < kStep); // the first one stays
}

TESSERA_TEST(a_state_older_than_all_kept_in_a_full_history_is_ignored_and_counted) {
    Fixture f(Config(0)); // 6 states
    f.interpolator.OnEnter(0, kMobType);
    for (uint64_t tick = 10; tick < 16; ++tick) {
        f.Feed(0, tick, static_cast<double>(tick));
    }
    TESSERA_CHECK(f.interpolator.StateCount(0) == 6);
    f.Feed(0, 5, 5.0);
    TESSERA_CHECK(f.interpolator.StateCount(0) == 6);
    TESSERA_CHECK(f.interpolator.StateTick(0, 0) == 10);
    TESSERA_CHECK(f.interpolator.Stats().states_ignored == 1);
    TESSERA_CHECK(f.interpolator.Stats().states == 6);
    // One that falls between the kept ones still gets in, pushing the oldest out.
    f.Feed(0, 12, 12.5); // a repeat: ignored
    TESSERA_CHECK(f.interpolator.Stats().states_ignored == 2);
}

TESSERA_TEST(the_oldest_state_leaves_when_the_history_is_full) {
    Fixture f(Config(0)); // 6 states
    f.interpolator.OnEnter(0, kMobType);
    for (uint64_t tick = 1; tick <= 7; ++tick) {
        f.Feed(0, tick, static_cast<double>(tick));
    }
    TESSERA_CHECK(f.interpolator.StateCount(0) == 6);
    for (uint32_t i = 0; i < 6; ++i) {
        TESSERA_CHECK(f.interpolator.StateTick(0, i) == 2 + i);
        TESSERA_CHECK(std::fabs(f.XAt(0, i) - static_cast<double>(2 + i)) < kStep);
    }
    TESSERA_CHECK(f.interpolator.Stats().states == 7);
    TESSERA_CHECK(f.interpolator.Stats().states_ignored == 0);

    // A late state that is newer than the oldest kept one gets in and the oldest leaves.
    f.interpolator.OnEnter(1, kMobType);
    for (uint64_t tick : {10, 12, 14, 16, 18, 20}) {
        f.Feed(1, tick, static_cast<double>(tick));
    }
    f.Feed(1, 11, 11.0);
    TESSERA_CHECK(f.interpolator.StateCount(1) == 6);
    TESSERA_CHECK(f.interpolator.StateTick(1, 0) == 11);
    TESSERA_CHECK(f.interpolator.StateTick(1, 5) == 20);
}

TESSERA_TEST(a_record_for_an_entity_that_never_entered_is_ignored) {
    Fixture f;
    f.Feed(5, 10, 1.0);
    TESSERA_CHECK(f.interpolator.StateCount(5) == 0);
    TESSERA_CHECK(f.interpolator.Stats().states == 0);
    TESSERA_CHECK(f.interpolator.Stats().states_ignored == 1);
    f.Feed(5000, 10, 1.0); // out of range
    TESSERA_CHECK(f.interpolator.Stats().states_ignored == 2);
    TESSERA_CHECK(f.interpolator.StateCount(5000) == 0);
}

TESSERA_TEST(leaving_frees_the_entity_and_a_reentering_id_starts_empty) {
    Fixture f;
    f.interpolator.OnEnter(3, kMobType);
    f.Feed(3, 10, 1.0);
    f.Feed(3, 11, 2.0);
    TESSERA_CHECK(f.interpolator.InUse() == 1);
    f.interpolator.OnLeave(3);
    TESSERA_CHECK(f.interpolator.InUse() == 0);
    TESSERA_CHECK(f.interpolator.StateCount(3) == 0);
    TESSERA_CHECK(f.interpolator.StateWords(3, 0) == nullptr);
    f.Feed(3, 12, 3.0); // for an entity that is gone
    TESSERA_CHECK(f.interpolator.StateCount(3) == 0);
    TESSERA_CHECK(f.interpolator.Stats().states_ignored == 1);

    // The id comes back for another entity: nothing of the old one is there.
    TESSERA_CHECK(f.interpolator.OnEnter(3, kMobType));
    TESSERA_CHECK(f.interpolator.StateCount(3) == 0);
    f.Feed(3, 13, 50.0);
    TESSERA_CHECK(f.interpolator.StateCount(3) == 1);
    TESSERA_CHECK(f.interpolator.StateTick(3, 0) == 13);

    // Entering again without leaving is the same: the old states do not carry over.
    TESSERA_CHECK(f.interpolator.OnEnter(3, kMobType));
    TESSERA_CHECK(f.interpolator.StateCount(3) == 0);
    TESSERA_CHECK(f.interpolator.InUse() == 1); // the old room was given back, not kept beside the new
    f.interpolator.OnLeave(3);
    f.interpolator.OnLeave(3); // twice is harmless
    TESSERA_CHECK(f.interpolator.InUse() == 0);
    f.interpolator.OnLeave(9999);
}

TESSERA_TEST(a_compact_id_reused_for_a_type_with_more_words_gets_a_chunk_of_that_size) {
    Fixture f;
    const uint32_t mob_words = f.schema.Find(kMobType)->words;
    const uint32_t score_words = f.schema.Find(kScoreType)->words;
    TESSERA_CHECK(mob_words > score_words);

    // Neighbours of the small type, in the pool the id will leave and the one it will join.
    for (uint32_t id = 4; id <= 6; ++id) {
        TESSERA_CHECK(f.interpolator.OnEnter(id, kScoreType));
        for (uint64_t tick = 1; tick <= 8; ++tick) {
            const std::vector<uint32_t> words = ScoreState(f.schema, static_cast<int64_t>(id * 100 + tick));
            f.interpolator.OnRecord(id, tick, words.data());
        }
    }
    f.interpolator.OnLeave(5);
    TESSERA_CHECK(f.interpolator.OnEnter(5, kMobType));
    for (uint64_t tick = 1; tick <= 8; ++tick) {
        f.Feed(5, tick, static_cast<double>(tick));
    }
    f.interpolator.OnEnter(7, kMobType);
    for (uint64_t tick = 1; tick <= 8; ++tick) {
        f.Feed(7, tick, static_cast<double>(100 + tick));
    }

    TESSERA_CHECK(f.interpolator.StateCount(5) == 8);
    for (uint32_t i = 0; i < 8; ++i) {
        const std::vector<uint32_t> expected = MobState(f.schema, static_cast<double>(i + 1));
        const uint32_t* stored = f.interpolator.StateWords(5, i);
        TESSERA_CHECK(stored != nullptr);
        bool same = stored != nullptr;
        for (uint32_t w = 0; same && w < mob_words; ++w) {
            same = stored[w] == expected[w];
        }
        TESSERA_CHECK(same);
        const std::vector<uint32_t> next = MobState(f.schema, static_cast<double>(101 + i));
        const uint32_t* other = f.interpolator.StateWords(7, i);
        bool next_same = other != nullptr;
        for (uint32_t w = 0; next_same && w < mob_words; ++w) {
            next_same = other[w] == next[w];
        }
        TESSERA_CHECK(next_same);
    }
    // The small neighbours were not touched.
    for (uint32_t id : {4u, 6u}) {
        for (uint32_t i = 0; i < 8; ++i) {
            const std::vector<uint32_t> expected =
                ScoreState(f.schema, static_cast<int64_t>(id * 100 + i + 1));
            const uint32_t* stored = f.interpolator.StateWords(id, i);
            TESSERA_CHECK(stored != nullptr && stored[0] == expected[0] && stored[1] == expected[1]);
        }
    }
}

TESSERA_TEST(reset_forgets_the_states_but_keeps_the_configuration) {
    Fixture f(Config(5, 4.0, 20));
    f.interpolator.OnEnter(0, kMobType);
    f.Feed(0, 10, 1.0);
    f.Feed(0, 10, 1.0); // counted as ignored
    TESSERA_CHECK(f.interpolator.Stats().states == 1 && f.interpolator.Stats().states_ignored == 1);
    f.interpolator.Reset(32);
    TESSERA_CHECK(f.interpolator.Enabled());
    TESSERA_CHECK(f.interpolator.Config().delay_ticks == 5);
    TESSERA_CHECK(f.interpolator.Config().snap_distance == 4.0);
    TESSERA_CHECK(f.interpolator.Config().max_gap_ticks == 20);
    TESSERA_CHECK(f.interpolator.StateCount(0) == 0);
    TESSERA_CHECK(f.interpolator.Stats().states == 0 && f.interpolator.Stats().states_ignored == 0);
    TESSERA_CHECK(f.interpolator.OnEnter(0, kMobType));

    f.interpolator.Reset(0); // room for nobody
    TESSERA_CHECK(f.interpolator.Enabled());
    TESSERA_CHECK(!f.interpolator.OnEnter(0, kMobType));

    f.interpolator.Clear();
    TESSERA_CHECK(!f.interpolator.Enabled());
    TESSERA_CHECK(f.interpolator.MemoryBytes() == 0);
}

TESSERA_TEST(memory_per_entity_follows_the_formula_and_does_not_grow_with_states) {
    const ReplicationSchema schema = MakeTestSchema();
    Interpolator interpolator(schema);
    TESSERA_CHECK(interpolator.MemoryBytes() == 0); // nothing before it is configured

    constexpr uint32_t kEntities = 1000;
    TESSERA_CHECK(interpolator.Configure(Config(), nullptr));
    interpolator.Reset(kEntities);
    for (uint32_t id = 0; id < kEntities; ++id) {
        TESSERA_CHECK(interpolator.OnEnter(id, kMobType));
    }
    const uint32_t words = schema.Find(kMobType)->words;
    const size_t chunk = interpolator.Capacity() * (sizeof(uint64_t) + sizeof(uint32_t) * words + 1);
    const size_t before = interpolator.MemoryBytes();
    TESSERA_CHECK(before >= kEntities * chunk);
    TESSERA_CHECK(before <= kEntities * (chunk + sizeof(void*) * 4 + 64) + 64 * 1024);

    for (uint64_t tick = 1; tick <= 1000; ++tick) {
        for (uint32_t id = 0; id < kEntities; ++id) {
            const std::vector<uint32_t> state = MobState(schema, static_cast<double>(tick % 100) * 0.01);
            interpolator.OnRecord(id, tick, state.data());
        }
    }
    TESSERA_CHECK(interpolator.MemoryBytes() == before);
    for (uint32_t id = 0; id < kEntities; ++id) {
        TESSERA_CHECK(interpolator.StateCount(id) == interpolator.Capacity());
    }
}

TESSERA_TEST(a_gap_above_the_limit_is_marked_unlinked_and_counted) {
    Fixture f(Config(2, 2.5, 8));
    f.interpolator.OnEnter(0, kMobType);
    f.interpolator.OnEnter(1, kMobType);
    f.Feed(0, 10, 1.0);
    f.Feed(0, 19, 1.0);
    TESSERA_CHECK(f.interpolator.StateCount(0) == 2);
    TESSERA_CHECK(!f.interpolator.Linked(0, 0));
    TESSERA_CHECK(!f.interpolator.Linked(0, 1));
    TESSERA_CHECK(f.interpolator.Stats().gap_holds == 1);
    TESSERA_CHECK(f.interpolator.Stats().snaps == 0);

    f.Feed(1, 10, 1.0);
    f.Feed(1, 18, 1.0); // exactly at the limit
    TESSERA_CHECK(f.interpolator.Linked(1, 1));
    TESSERA_CHECK(f.interpolator.Stats().gap_holds == 1);
}

TESSERA_TEST(a_jump_above_the_snap_distance_is_marked_unlinked_and_counted) {
    Fixture f(Config(2, 2.5, 8));
    f.interpolator.OnEnter(0, kMobType);
    f.interpolator.OnEnter(1, kMobType);
    f.Feed(0, 10, 0.0);
    f.Feed(0, 11, 3.0);
    TESSERA_CHECK(!f.interpolator.Linked(0, 1));
    TESSERA_CHECK(f.interpolator.Stats().snaps == 1);
    TESSERA_CHECK(f.interpolator.Stats().gap_holds == 0);

    f.Feed(1, 10, 0.0);
    f.Feed(1, 11, 2.4);
    TESSERA_CHECK(f.interpolator.Linked(1, 1));
    TESSERA_CHECK(f.interpolator.Stats().snaps == 1);

    // Along any axis, and in all three together.
    f.interpolator.OnEnter(2, kMobType);
    {
        const std::vector<uint32_t> a = MobState(f.schema, 0.0, 0.0, 0.0);
        const std::vector<uint32_t> b = MobState(f.schema, 1.5, 1.5, 1.5); // 2.6 m away
        f.interpolator.OnRecord(2, 10, a.data());
        f.interpolator.OnRecord(2, 11, b.data());
    }
    TESSERA_CHECK(!f.interpolator.Linked(2, 1));
    TESSERA_CHECK(f.interpolator.Stats().snaps == 2);

    // A type with two positions: the distances count together.
    struct TwoColumns {
        float x[kPageSlots];
        float y[kPageSlots];
        float z[kPageSlots];
        float x2[kPageSlots];
        float y2[kPageSlots];
        float z2[kPageSlots];
    };
    ReplicationSchema two;
    TESSERA_CHECK(two.AddType(
        0, {PropertyDescriptor::Position(TESSERA_COLUMN(TwoColumns, x), TESSERA_COLUMN(TwoColumns, y),
                                         TESSERA_COLUMN(TwoColumns, z), 0.01f, 10000.0f),
            PropertyDescriptor::Position(TESSERA_COLUMN(TwoColumns, x2), TESSERA_COLUMN(TwoColumns, y2),
                                         TESSERA_COLUMN(TwoColumns, z2), 0.01f, 10000.0f)}));
    Interpolator interpolator(two);
    interpolator.Configure(Config(2, 2.5, 8), nullptr);
    interpolator.Reset(4);
    interpolator.OnEnter(0, 0);
    interpolator.OnEnter(1, 0);
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
    // 1.8 m on each of the two: 2.55 m together, above 2.5, though each alone is below.
    const std::vector<uint32_t> start = make(0.0f, 0.0f);
    const std::vector<uint32_t> farther = make(1.8f, 1.8f);
    const std::vector<uint32_t> nearer = make(1.8f, 0.0f);
    interpolator.OnRecord(0, 10, start.data());
    interpolator.OnRecord(0, 11, farther.data());
    TESSERA_CHECK(!interpolator.Linked(0, 1));
    TESSERA_CHECK(interpolator.Stats().snaps == 1);
    interpolator.OnRecord(1, 10, start.data());
    interpolator.OnRecord(1, 11, nearer.data());
    TESSERA_CHECK(interpolator.Linked(1, 1));
}

TESSERA_TEST(a_late_state_in_the_middle_recomputes_the_link_after_it_without_counting) {
    Fixture f(Config(2, 2.5, 8));
    f.interpolator.OnEnter(0, kMobType);
    f.Feed(0, 10, 1.0);
    f.Feed(0, 19, 1.0);
    TESSERA_CHECK(!f.interpolator.Linked(0, 1));
    TESSERA_CHECK(f.interpolator.Stats().gap_holds == 1);

    f.Feed(0, 14, 1.0); // the middle: both gaps are now within the limit
    TESSERA_CHECK(f.interpolator.StateCount(0) == 3);
    TESSERA_CHECK(f.interpolator.StateTick(0, 1) == 14);
    TESSERA_CHECK(f.interpolator.Linked(0, 1));
    TESSERA_CHECK(f.interpolator.Linked(0, 2));
    TESSERA_CHECK(f.interpolator.Stats().gap_holds == 1);
    TESSERA_CHECK(f.interpolator.Stats().snaps == 0);
    TESSERA_CHECK(f.interpolator.Stats().states == 3);

    // A late state that makes things worse is not counted either (it did not arrive last).
    f.interpolator.OnEnter(1, kMobType);
    f.Feed(1, 10, 0.0);
    f.Feed(1, 12, 0.0);
    f.Feed(1, 11, 5.0); // 5 m out of line: unlinked on both sides
    TESSERA_CHECK(!f.interpolator.Linked(1, 1));
    TESSERA_CHECK(!f.interpolator.Linked(1, 2));
    TESSERA_CHECK(f.interpolator.Stats().snaps == 0);
}
