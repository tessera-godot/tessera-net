// SPDX-License-Identifier: Apache-2.0
// What interpolation costs: nothing when it is off, no allocation while states arrive or frames are shown,
// and a memory use per entity that follows from the number of states kept. The time is measured by
// bench/bench_interpolation.cpp, in a build that is optimized.
#include "allocation_counter.hpp"
#include "network_test_support.hpp"
#include "tessera/net/interpolator.hpp"
#include "tessera/net/replication/property_codecs.hpp"
#include "test_support.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

using namespace tessera::net;
using allocation_test::AllocationScope;

namespace {

// A player-like type: a position and a 64-bit number, which makes 5 words (the player of the example
// server has 5 as well), and a mob-like type with a position only, which makes 3.
struct CostColumns {
    float x[kPageSlots];
    float y[kPageSlots];
    float z[kPageSlots];
    int64_t score[kPageSlots];
};

constexpr uint32_t kPlayerLike = 0;
constexpr uint32_t kMobLike = 1;

ReplicationSchema MakeCostSchema() {
    ReplicationSchema schema;
    schema.AddType(kPlayerLike,
                   {PropertyDescriptor::Position(TESSERA_COLUMN(CostColumns, x),
                                                 TESSERA_COLUMN(CostColumns, y),
                                                 TESSERA_COLUMN(CostColumns, z), 0.01f, 2048.0f),
                    PropertyDescriptor::Integer(TESSERA_COLUMN(CostColumns, score), true,
                                                std::numeric_limits<int64_t>::min(),
                                                std::numeric_limits<int64_t>::max())});
    schema.AddType(kMobLike,
                   {PropertyDescriptor::Position(TESSERA_COLUMN(CostColumns, x),
                                                 TESSERA_COLUMN(CostColumns, y),
                                                 TESSERA_COLUMN(CostColumns, z), 0.01f, 2048.0f)});
    return schema;
}

std::vector<uint32_t> StateOf(const ReplicationSchema& schema, uint32_t type, float x) {
    auto columns = std::make_unique<CostColumns>();
    columns->x[0] = x;
    columns->y[0] = 1.0f;
    columns->z[0] = 2.0f;
    columns->score[0] = 7;
    const TypeSchema* description = schema.Find(type);
    std::vector<uint32_t> words(description->words, 0);
    for (const PropertyDescriptor& property : description->properties) {
        QuantizeProperty(property, reinterpret_cast<const std::byte*>(columns.get()), 0,
                         words.data() + property.word_offset);
    }
    return words;
}

InterpolationConfig Delay(uint32_t ticks) {
    InterpolationConfig config;
    config.delay_ticks = ticks;
    return config;
}

// An interpolator with `count` entities of `type` that are visible, and no states yet.
struct Crowd {
    const ReplicationSchema& schema;
    Interpolator interpolator;
    uint32_t count;

    Crowd(const ReplicationSchema& s, uint32_t type, uint32_t entities, uint32_t delay)
        : schema(s), interpolator(s), count(entities) {
        TESSERA_CHECK(interpolator.Configure(Delay(delay), nullptr));
        interpolator.Reset(entities);
        for (uint32_t id = 0; id < entities; ++id) {
            TESSERA_CHECK(interpolator.OnEnter(id, type));
        }
    }
};

} // namespace

TESSERA_TEST(without_a_config_the_interpolator_reserves_nothing) {
    const ReplicationSchema schema = MakeCostSchema();
    Interpolator interpolator(schema);
    TESSERA_CHECK(interpolator.MemoryBytes() == 0);
    const std::vector<uint32_t> state = StateOf(schema, kPlayerLike, 5.0f);
    {
        AllocationScope scope;
        interpolator.Reset(1000);
        TESSERA_CHECK(!interpolator.OnEnter(0, kPlayerLike));
        interpolator.OnRecord(0, 1, state.data());
        interpolator.OnBuffer(1);
        interpolator.Advance(1.0);
        double out[3];
        TESSERA_CHECK(!interpolator.Read(0, 0, out) && interpolator.HeldWords(0) == nullptr);
        TESSERA_CHECK(scope.Count() == 0);
    }
    TESSERA_CHECK(interpolator.MemoryBytes() == 0);

    // A client that never asked for it: 100 buffers later it still holds nothing for interpolation.
    network_test::ClientHarness h;
    network_test::ReplicationSource source(h.schema);
    const EntityId entity = source.Spawn(10.0f, 0.0f, 0.0f);
    const std::vector<VisibilityEvent> enter{network_test::Enter(entity, 0)};
    source.Show(enter);
    h.Step();
    h.Accept();
    h.Step();
    h.SendBatch(1, 1, enter);
    for (uint64_t tick = 1; tick <= 100; ++tick) {
        source.MoveX(entity, 10.0f + static_cast<float>(tick) * 0.1f);
        h.SendStateBuffer(tick, 1, source.Write(tick), 0);
        h.Step();
    }
    TESSERA_CHECK(h.client.Stats().buffers_applied == 100);
    TESSERA_CHECK(h.client.InterpolationMemoryBytes() == 0);
    TESSERA_CHECK(!h.client.InterpolationSet());
}

TESSERA_TEST(feeding_advancing_and_reading_states_does_not_allocate) {
    const ReplicationSchema schema = MakeCostSchema();
    constexpr uint32_t kEntities = 1000;
    Crowd crowd(schema, kPlayerLike, kEntities, 2);
    Interpolator& interpolator = crowd.interpolator;
    // Two states, a little apart, that take turns; a tick of every entity is one record each.
    std::vector<uint32_t> states[2] = {StateOf(schema, kPlayerLike, 5.0f),
                                       StateOf(schema, kPlayerLike, 5.2f)};
    uint64_t tick = 0;
    for (; tick < 40; ++tick) { // warm up: every history has been full
        for (uint32_t id = 0; id < kEntities; ++id) {
            interpolator.OnRecord(id, tick, states[tick % 2].data());
        }
        interpolator.OnBuffer(tick);
        interpolator.Advance(1.0);
    }
    double checksum = 0.0;
    uint64_t allocations = 0;
    {
        AllocationScope scope;
        for (int round = 0; round < 500; ++round, ++tick) {
            for (uint32_t id = 0; id < kEntities; ++id) {
                interpolator.OnRecord(id, tick, states[tick % 2].data());
            }
            interpolator.OnBuffer(tick);
            interpolator.Advance(0.5);
            interpolator.Advance(0.5);
            double out[3];
            for (uint32_t id = 0; id < kEntities; id += 7) {
                if (interpolator.Read(id, 0, out)) {
                    checksum += out[0];
                }
                if (interpolator.HeldWords(id) != nullptr) {
                    checksum += 1.0;
                }
            }
        }
        allocations = scope.Count();
    }
    TESSERA_CHECK(allocations == 0);
    TESSERA_CHECK(checksum > 0.0); // it did read
}

TESSERA_TEST(memory_per_entity_stays_within_the_recorded_limit) {
    const ReplicationSchema schema = MakeCostSchema();
    TESSERA_CHECK(schema.Find(kPlayerLike)->words == 5 && schema.Find(kMobLike)->words == 3);
    for (const uint32_t count : {1000u, 10000u}) {
        Crowd players(schema, kPlayerLike, count, 2);
        const double per_player = static_cast<double>(players.interpolator.MemoryBytes()) / count;
        TESSERA_CHECK(per_player <= 384.0);
        Crowd mobs(schema, kMobLike, count, 2);
        const double per_mob = static_cast<double>(mobs.interpolator.MemoryBytes()) / count;
        TESSERA_CHECK(per_mob <= 320.0);
    }
    Crowd slow(schema, kPlayerLike, 1000, 30); // the longest delay: 36 states
    const double per_slow = static_cast<double>(slow.interpolator.MemoryBytes()) / 1000.0;
    TESSERA_CHECK(per_slow <= 1536.0);

    // What MemoryBytes says is what was asked of the allocator.
    const ReplicationSchema counted_schema = MakeCostSchema();
    uint64_t held = 0;
    size_t reported = 0;
    {
        AllocationScope scope;
        Crowd crowd(counted_schema, kMobLike, 5000, 2); // 3 words, less than the most the schema has
        held = scope.LiveBytes();
        reported = crowd.interpolator.MemoryBytes();
    }
    TESSERA_CHECK(held > 0);
    TESSERA_CHECK(static_cast<double>(reported) >= 0.97 * static_cast<double>(held));
    TESSERA_CHECK(static_cast<double>(reported) <= 1.03 * static_cast<double>(held));
}

TESSERA_TEST(memory_grows_linearly_with_the_delay_and_with_the_entities) {
    const ReplicationSchema schema = MakeCostSchema();
    Crowd small(schema, kPlayerLike, 2000, 2);
    Crowd big(schema, kPlayerLike, 4000, 2);
    const double ratio = static_cast<double>(big.interpolator.MemoryBytes()) /
                         static_cast<double>(small.interpolator.MemoryBytes());
    TESSERA_CHECK(ratio >= 1.95 && ratio <= 2.05); // twice the entities, twice the memory

    // A state takes 8 bytes of tick, 20 of words and 1 of mark; an entity also takes its own record.
    Crowd short_delay(schema, kPlayerLike, 2000, 2);   // 8 states
    Crowd long_delay(schema, kPlayerLike, 2000, 14);   // 20 states
    const double expected = (20.0 * 29.0 + sizeof(void*) * 3) / (8.0 * 29.0 + sizeof(void*) * 3);
    const double measured = static_cast<double>(long_delay.interpolator.MemoryBytes()) /
                            static_cast<double>(short_delay.interpolator.MemoryBytes());
    TESSERA_CHECK(std::fabs(measured - expected) <= 0.05 * expected);
    TESSERA_CHECK(measured > 2.0);
}
