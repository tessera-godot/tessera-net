// SPDX-License-Identifier: Apache-2.0
#include "replication_test_support.hpp"
#include "tessera/net/replication/property_codecs.hpp"
#include "test_support.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <vector>

using namespace tessera::net;
using namespace replication_test;

namespace {

constexpr double kPi = 3.14159265358979323846;

// The position and angle descriptors of the test schema (property 0 and 1 of the mob type).
const PropertyDescriptor& PositionOf(const ReplicationSchema& schema) {
    return schema.Find(kMobType)->properties[0];
}
const PropertyDescriptor& AngleOf(const ReplicationSchema& schema) {
    return schema.Find(kMobType)->properties[1];
}

struct Words3 {
    uint32_t w[3];
};

std::vector<uint8_t> WriteDeltaBytes(const PropertyDescriptor& d, const uint32_t* base, const uint32_t* cur,
                                     uint64_t* bits) {
    std::vector<uint8_t> bytes;
    BitWriter writer(bytes);
    WriteDelta(d, base, cur, writer);
    if (bits != nullptr) {
        *bits = writer.BitCount();
    }
    writer.Finish();
    return bytes;
}

// Quantizes one float triple through the real column reader.
Words3 QuantizePosition(const PropertyDescriptor& d, float x, float y, float z) {
    auto columns = std::make_unique<TestColumns>();
    columns->x[5] = x;
    columns->y[5] = y;
    columns->z[5] = z;
    Words3 words{};
    QuantizeProperty(d, reinterpret_cast<const std::byte*>(columns.get()), 5, words.w);
    return words;
}

uint32_t QuantizeAngle(const PropertyDescriptor& d, float radians) {
    auto columns = std::make_unique<TestColumns>();
    columns->yaw[2] = radians;
    uint32_t word = 0xDEADBEEF;
    QuantizeProperty(d, reinterpret_cast<const std::byte*>(columns.get()), 2, &word);
    return word;
}

} // namespace

TESSERA_TEST(position_quantizes_extremes_and_zero) {
    const ReplicationSchema schema = MakeTestSchema();
    const PropertyDescriptor& d = PositionOf(schema);
    const Words3 w = QuantizePosition(d, -10000.0f, 10000.0f, 0.0f);
    TESSERA_CHECK(w.w[0] == 0);
    TESSERA_CHECK(w.w[1] == 2'000'000);
    TESSERA_CHECK(w.w[2] == 1'000'000);
}

TESSERA_TEST(position_out_of_range_is_limited_and_nan_is_center) {
    const ReplicationSchema schema = MakeTestSchema();
    const PropertyDescriptor& d = PositionOf(schema);
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Words3 w = QuantizePosition(d, 20000.0f, -inf, inf);
    TESSERA_CHECK(w.w[0] == 2'000'000 && w.w[1] == 0 && w.w[2] == 2'000'000);
    const Words3 lows = QuantizePosition(d, -20000.0f, nan, -0.0f);
    TESSERA_CHECK(lows.w[0] == 0 && lows.w[1] == 1'000'000 && lows.w[2] == 1'000'000);
    const Words3 zero = QuantizePosition(d, 0.0f, 0.0f, 0.0f);
    TESSERA_CHECK(zero.w[2] == lows.w[2]); // -0.0 and 0.0 give the same value
}

TESSERA_TEST(position_quantization_error_is_at_most_half_a_step) {
    const ReplicationSchema schema = MakeTestSchema();
    const PropertyDescriptor& d = PositionOf(schema);
    std::mt19937 random(7);
    std::uniform_real_distribution<float> value(-10000.0f, 10000.0f);
    for (int i = 0; i < 10000; ++i) {
        const float x = value(random), y = value(random), z = value(random);
        const Words3 w = QuantizePosition(d, x, y, z);
        double back[3];
        DequantizePosition(d, w.w, back);
        TESSERA_CHECK(std::fabs(back[0] - x) <= 0.005 + 1e-9);
        TESSERA_CHECK(std::fabs(back[1] - y) <= 0.005 + 1e-9);
        TESSERA_CHECK(std::fabs(back[2] - z) <= 0.005 + 1e-9);
    }
}

TESSERA_TEST(position_absolute_round_trip_at_the_extremes) {
    const ReplicationSchema schema = MakeTestSchema();
    const PropertyDescriptor& d = PositionOf(schema);
    const uint32_t words[3] = {0, 2'000'000, 1'000'000};
    std::vector<uint8_t> bytes;
    BitWriter writer(bytes);
    WriteAbsolute(d, words, writer);
    TESSERA_CHECK(writer.BitCount() == 63);
    writer.Finish();
    BitReader reader(bytes);
    uint32_t back[3] = {9, 9, 9};
    TESSERA_CHECK(ReadAbsolute(d, reader, back));
    TESSERA_CHECK(back[0] == 0 && back[1] == 2'000'000 && back[2] == 1'000'000);
}

TESSERA_TEST(position_delta_round_trip_across_the_center_and_the_extremes) {
    const ReplicationSchema schema = MakeTestSchema();
    const PropertyDescriptor& d = PositionOf(schema);
    const uint32_t values[] = {0, 1, 999'999, 1'000'000, 1'000'001, 1'999'999, 2'000'000};
    for (const uint32_t bx : values) {
        for (const uint32_t cx : values) {
            const uint32_t base[3] = {bx, cx, 1'000'000};
            const uint32_t cur[3] = {cx, bx, 0};
            const std::vector<uint8_t> bytes = WriteDeltaBytes(d, base, cur, nullptr);
            BitReader reader(bytes);
            uint32_t back[3] = {0, 0, 0};
            TESSERA_CHECK(ReadDelta(d, reader, base, back));
            TESSERA_CHECK(back[0] == cur[0] && back[1] == cur[1] && back[2] == cur[2]);
        }
    }
}

TESSERA_TEST(small_position_move_costs_few_bits) {
    const ReplicationSchema schema = MakeTestSchema();
    const PropertyDescriptor& d = PositionOf(schema);
    const uint32_t base[3] = {1'000'000, 1'000'000, 1'000'000};
    const uint32_t cur[3] = {1'000'020, 999'980, 1'000'020}; // 20 steps on every axis
    uint64_t bits = 0;
    WriteDeltaBytes(d, base, cur, &bits);
    TESSERA_CHECK(bits == 1 + 3 * 8); // the flag, then 8 bits per axis
}

TESSERA_TEST(large_position_move_falls_back_to_absolute) {
    const ReplicationSchema schema = MakeTestSchema();
    const PropertyDescriptor& d = PositionOf(schema);
    const uint32_t base[3] = {0, 0, 0};
    const uint32_t cur[3] = {1'000'000, 1'000'000, 1'000'000};
    uint64_t bits = 0;
    const std::vector<uint8_t> bytes = WriteDeltaBytes(d, base, cur, &bits);
    TESSERA_CHECK(bits == 1 + 3 * 21); // flag plus three full values, cheaper than three long deltas
    BitReader reader(bytes);
    uint32_t back[3] = {0, 0, 0};
    TESSERA_CHECK(ReadDelta(d, reader, base, back));
    TESSERA_CHECK(back[0] == cur[0] && back[1] == cur[1] && back[2] == cur[2]);
}

TESSERA_TEST(position_delta_tie_prefers_delta) {
    // Range 5000 at 1 cm gives 20-bit absolute values, so three full values and three 20-bit deltas both
    // cost 61.
    ReplicationSchema schema;
    TESSERA_CHECK(schema.AddType(0, {PropertyDescriptor::Position(ColumnRef{0, 4}, ColumnRef{4, 4},
                                                                  ColumnRef{8, 4}, 0.01f, 5000.0f)}));
    const PropertyDescriptor& d = schema.Find(0)->properties[0];
    TESSERA_CHECK(d.absolute_bits == 20);
    const uint32_t base[3] = {100'000, 100'000, 100'000};
    const uint32_t cur[3] = {103'000, 103'000, 103'000}; // delta 3000 -> zigzag 6000 -> 20 bits each
    uint64_t bits = 0;
    const std::vector<uint8_t> bytes = WriteDeltaBytes(d, base, cur, &bits);
    TESSERA_CHECK(bits == 61);
    TESSERA_CHECK((bytes[0] & 1) == 0); // flag bit 0 means delta
}

TESSERA_TEST(position_delta_that_leaves_the_range_is_rejected) {
    const ReplicationSchema schema = MakeTestSchema();
    const PropertyDescriptor& d = PositionOf(schema);
    std::vector<uint8_t> bytes;
    BitWriter writer(bytes);
    writer.WriteBool(false);
    writer.WriteVarInt(5);
    writer.WriteVarInt(0);
    writer.WriteVarInt(0);
    writer.Finish();
    const uint32_t base[3] = {2'000'000, 0, 0}; // +5 would pass the largest value
    BitReader reader(bytes);
    uint32_t back[3] = {7, 7, 7};
    TESSERA_CHECK(!ReadDelta(d, reader, base, back));
    TESSERA_CHECK(back[0] == 7 && back[1] == 7 && back[2] == 7);
}

TESSERA_TEST(angle_wraps_across_zero_and_two_pi) {
    const ReplicationSchema schema = MakeTestSchema();
    const PropertyDescriptor& d = AngleOf(schema);
    TESSERA_CHECK(QuantizeAngle(d, 0.0f) == 0);
    TESSERA_CHECK(QuantizeAngle(d, static_cast<float>(2.0 * kPi)) == 0);
    TESSERA_CHECK(QuantizeAngle(d, static_cast<float>(-2.0 * kPi)) == 0);
    TESSERA_CHECK(QuantizeAngle(d, static_cast<float>(kPi)) == 512);
    TESSERA_CHECK(QuantizeAngle(d, static_cast<float>(-kPi)) == 512);
    const uint32_t negative = QuantizeAngle(d, -0.5f); // -0.5 rad wraps to 2*pi - 0.5
    TESSERA_CHECK(negative ==
                  static_cast<uint32_t>(std::llround((1.0 - 0.5 / (2.0 * kPi)) * 1024.0)) % 1024u);
    // The circular difference from 1023 to 0 is +1, written as a flag and the smallest var-int.
    const uint32_t base = 1023, cur = 0;
    uint64_t bits = 0;
    const std::vector<uint8_t> bytes = WriteDeltaBytes(d, &base, &cur, &bits);
    TESSERA_CHECK(bits == 1 + VarIntBits(1));
    BitReader reader(bytes);
    uint32_t back = 55;
    TESSERA_CHECK(ReadDelta(d, reader, &base, &back));
    TESSERA_CHECK(back == 0);
}

TESSERA_TEST(angle_that_is_not_finite_is_zero) {
    const ReplicationSchema schema = MakeTestSchema();
    const PropertyDescriptor& d = AngleOf(schema);
    TESSERA_CHECK(QuantizeAngle(d, std::numeric_limits<float>::quiet_NaN()) == 0);
    TESSERA_CHECK(QuantizeAngle(d, std::numeric_limits<float>::infinity()) == 0);
    TESSERA_CHECK(QuantizeAngle(d, -std::numeric_limits<float>::infinity()) == 0);
}

TESSERA_TEST(angle_absolute_and_delta_round_trip_for_every_pair) {
    ReplicationSchema schema;
    TESSERA_CHECK(schema.AddType(0, {PropertyDescriptor::Angle(ColumnRef{0, 4}, 4)}));
    const PropertyDescriptor& d = schema.Find(0)->properties[0];
    for (uint32_t base = 0; base < 16; ++base) {
        for (uint32_t cur = 0; cur < 16; ++cur) {
            const std::vector<uint8_t> bytes = WriteDeltaBytes(d, &base, &cur, nullptr);
            BitReader reader(bytes);
            uint32_t back = 99;
            TESSERA_CHECK(ReadDelta(d, reader, &base, &back));
            TESSERA_CHECK(back == cur);
        }
        std::vector<uint8_t> absolute;
        BitWriter writer(absolute);
        WriteAbsolute(d, &base, writer);
        writer.Finish();
        BitReader reader(absolute);
        uint32_t back = 99;
        TESSERA_CHECK(ReadAbsolute(d, reader, &back));
        TESSERA_CHECK(back == base);
    }
}

TESSERA_TEST(dequantize_returns_the_step_centers) {
    const ReplicationSchema schema = MakeTestSchema();
    const PropertyDescriptor& d = PositionOf(schema);
    const uint32_t words[3] = {0, 1'000'000, 2'000'000};
    double out[3];
    DequantizePosition(d, words, out);
    // The step is a float, so a value is a whole number of steps of exactly that size from -range.
    const double step = static_cast<double>(d.step);
    TESSERA_CHECK(std::fabs(out[0] + 10000.0) < 1e-9);
    TESSERA_CHECK(std::fabs(out[1] - (1'000'000 * step - 10000.0)) < 1e-9);
    TESSERA_CHECK(std::fabs(out[2] - (2'000'000 * step - 10000.0)) < 1e-9);
    TESSERA_CHECK(std::fabs(out[2] - 10000.0) < step / 2); // the top step is within half a step of +range
    const uint32_t quarter = 256;
    TESSERA_CHECK(std::fabs(DequantizeAngle(AngleOf(schema), &quarter) - kPi / 2.0) < 1e-12);
}

TESSERA_TEST(truncated_reads_fail_and_leave_words_untouched) {
    const ReplicationSchema schema = MakeTestSchema();
    const PropertyDescriptor& d = PositionOf(schema);
    const uint32_t base[3] = {1'000'000, 1'000'000, 1'000'000};
    const uint32_t cur[3] = {1'000'020, 999'980, 1'000'020};
    std::vector<uint8_t> bytes = WriteDeltaBytes(d, base, cur, nullptr);
    TESSERA_CHECK(bytes.size() == 4);
    bytes.resize(2);
    BitReader reader(bytes);
    uint32_t back[3] = {7, 7, 7};
    TESSERA_CHECK(!ReadDelta(d, reader, base, back));
    TESSERA_CHECK(back[0] == 7 && back[1] == 7 && back[2] == 7);

    std::vector<uint8_t> absolute;
    BitWriter writer(absolute);
    const uint32_t words[3] = {1, 2, 3};
    WriteAbsolute(d, words, writer);
    writer.Finish();
    absolute.resize(absolute.size() - 1);
    BitReader short_reader(absolute);
    TESSERA_CHECK(!ReadAbsolute(d, short_reader, back));
    TESSERA_CHECK(back[0] == 7 && back[1] == 7 && back[2] == 7);
}

TESSERA_TEST(absolute_read_rejects_an_ordinal_above_the_maximum) {
    const ReplicationSchema schema = MakeTestSchema();
    const PropertyDescriptor& d = PositionOf(schema);
    std::vector<uint8_t> bytes;
    BitWriter writer(bytes);
    writer.WriteBits(2'000'001, 21); // 21 bits can hold it, but the largest allowed value is 2,000,000
    writer.WriteBits(0, 21);
    writer.WriteBits(0, 21);
    writer.Finish();
    BitReader reader(bytes);
    uint32_t back[3] = {7, 7, 7};
    TESSERA_CHECK(!ReadAbsolute(d, reader, back));
    TESSERA_CHECK(back[0] == 7);
}

// ---- Integer, bit mask and enum ---------------------------------------------------------------------

namespace {

// One column of every integer size, to check that each is read with the right width and sign.
struct IntColumns {
    int8_t i8[8];
    uint8_t u8[8];
    int16_t i16[8];
    uint16_t u16[8];
    int32_t i32[8];
    uint32_t u32[8];
    int64_t i64[8];
    uint64_t u64[8];
};

const PropertyDescriptor& HealthOf(const ReplicationSchema& schema) {
    return schema.Find(kMobType)->properties[2];
}
const PropertyDescriptor& FlagsOf(const ReplicationSchema& schema) {
    return schema.Find(kMobType)->properties[3];
}
const PropertyDescriptor& StateOf(const ReplicationSchema& schema) {
    return schema.Find(kMobType)->properties[4];
}
const PropertyDescriptor& ScoreOf(const ReplicationSchema& schema) {
    return schema.Find(kScoreType)->properties[0];
}

struct Words2 {
    uint32_t w[2];
};

Words2 QuantizeFrom(const PropertyDescriptor& d, const void* columns, uint32_t slot) {
    Words2 words{{0xDEADBEEF, 0xDEADBEEF}};
    QuantizeProperty(d, static_cast<const std::byte*>(columns), slot, words.w);
    return words;
}

Words2 QuantizeInt(const PropertyDescriptor& d, int64_t value) {
    auto columns = std::make_unique<TestColumns>();
    columns->health[3] = static_cast<int32_t>(value);
    columns->score[3] = value;
    return QuantizeFrom(d, columns.get(), 3);
}

uint64_t AbsoluteBits(const PropertyDescriptor& d, const uint32_t* words) {
    std::vector<uint8_t> bytes;
    BitWriter writer(bytes);
    WriteAbsolute(d, words, writer);
    return writer.BitCount();
}

} // namespace

TESSERA_TEST(integer_reads_every_element_size_and_signedness) {
    IntColumns columns{};
    columns.i8[1] = -128;
    columns.u8[1] = 255;
    columns.i16[1] = -32768;
    columns.u16[1] = 65535;
    columns.i32[1] = -2147483647 - 1;
    columns.u32[1] = 4294967295u;
    columns.i64[1] = std::numeric_limits<int64_t>::min() + 5;
    columns.u64[1] = 12345678901234567ull;
    ReplicationSchema schema;
    const int64_t i32_min = std::numeric_limits<int32_t>::min();
    TESSERA_CHECK(schema.AddType(
        0, {PropertyDescriptor::Integer(TESSERA_COLUMN(IntColumns, i8), true, -128, 127),
            PropertyDescriptor::Integer(TESSERA_COLUMN(IntColumns, u8), false, 0, 255),
            PropertyDescriptor::Integer(TESSERA_COLUMN(IntColumns, i16), true, -32768, 32767),
            PropertyDescriptor::Integer(TESSERA_COLUMN(IntColumns, u16), false, 0, 65535),
            PropertyDescriptor::Integer(TESSERA_COLUMN(IntColumns, i32), true, i32_min, 2147483647),
            PropertyDescriptor::Integer(TESSERA_COLUMN(IntColumns, u32), false, 0, 4294967295ll),
            PropertyDescriptor::Integer(TESSERA_COLUMN(IntColumns, i64), true,
                                        std::numeric_limits<int64_t>::min(),
                                        std::numeric_limits<int64_t>::max()),
            PropertyDescriptor::Integer(TESSERA_COLUMN(IntColumns, u64), false, 0,
                                        std::numeric_limits<int64_t>::max())}));
    const int64_t expected[] = {-128,
                                255,
                                -32768,
                                65535,
                                i32_min,
                                4294967295ll,
                                std::numeric_limits<int64_t>::min() + 5,
                                12345678901234567ll};
    const TypeSchema* type = schema.Find(0);
    for (size_t i = 0; i < 8; ++i) {
        const Words2 words = QuantizeFrom(type->properties[i], &columns, 1);
        TESSERA_CHECK(IntegerValue(type->properties[i], words.w) == expected[i]);
    }
}

TESSERA_TEST(integer_is_limited_to_its_range) {
    const ReplicationSchema schema = MakeTestSchema();
    TESSERA_CHECK(IntegerValue(HealthOf(schema), QuantizeInt(HealthOf(schema), 5000).w) == 1000);
    TESSERA_CHECK(IntegerValue(HealthOf(schema), QuantizeInt(HealthOf(schema), -5000).w) == -1000);
    TESSERA_CHECK(IntegerValue(HealthOf(schema), QuantizeInt(HealthOf(schema), -1000).w) == -1000);
    TESSERA_CHECK(QuantizeInt(HealthOf(schema), -1000).w[0] == 0); // the minimum is ordinal 0
    TESSERA_CHECK(QuantizeInt(HealthOf(schema), 1000).w[0] == 2000);
}

TESSERA_TEST(integer_delta_across_the_full_64_bit_range) {
    const ReplicationSchema schema = MakeTestSchema();
    const PropertyDescriptor& d = ScoreOf(schema);
    const int64_t values[] = {
        std::numeric_limits<int64_t>::min(),     std::numeric_limits<int64_t>::min() + 1, -1, 0, 1,
        std::numeric_limits<int64_t>::max() - 1, std::numeric_limits<int64_t>::max()};
    for (const int64_t from : values) {
        for (const int64_t to : values) {
            const Words2 base = QuantizeInt(d, from);
            const Words2 cur = QuantizeInt(d, to);
            if (base.w[0] == cur.w[0] && base.w[1] == cur.w[1]) {
                continue; // a delta is only written when the states differ
            }
            const std::vector<uint8_t> bytes = WriteDeltaBytes(d, base.w, cur.w, nullptr);
            BitReader reader(bytes);
            uint32_t back[2] = {0, 0};
            TESSERA_CHECK(ReadDelta(d, reader, base.w, back));
            TESSERA_CHECK(IntegerValue(d, back) == to);
        }
    }
}

TESSERA_TEST(integer_picks_the_smaller_of_delta_and_absolute) {
    const ReplicationSchema schema = MakeTestSchema();
    const PropertyDescriptor& health = HealthOf(schema);
    uint64_t bits = 0;
    WriteDeltaBytes(health, QuantizeInt(health, 10).w, QuantizeInt(health, 11).w, &bits);
    TESSERA_CHECK(bits == 1 + VarIntBits(1));
    const PropertyDescriptor& score = ScoreOf(schema);
    const Words2 base = QuantizeInt(score, 0);
    const Words2 cur = QuantizeInt(score, 1'000'000'000'000'000'000ll);
    const std::vector<uint8_t> bytes = WriteDeltaBytes(score, base.w, cur.w, &bits);
    TESSERA_CHECK(bits == 1 + 64); // far too big a jump for a short delta
    BitReader reader(bytes);
    uint32_t back[2] = {0, 0};
    TESSERA_CHECK(ReadDelta(score, reader, base.w, back));
    TESSERA_CHECK(IntegerValue(score, back) == 1'000'000'000'000'000'000ll);
}

TESSERA_TEST(integer_absolute_read_rejects_out_of_range) {
    const ReplicationSchema schema = MakeTestSchema();
    const PropertyDescriptor& d = HealthOf(schema);
    std::vector<uint8_t> bytes;
    BitWriter writer(bytes);
    writer.WriteBits(2001, 11); // 11 bits hold up to 2047, but the largest ordinal is 2000
    writer.Finish();
    BitReader reader(bytes);
    uint32_t back[2] = {7, 7};
    TESSERA_CHECK(!ReadAbsolute(d, reader, back));
    TESSERA_CHECK(back[0] == 7 && back[1] == 7);
    // A delta that leaves the range is rejected as well.
    std::vector<uint8_t> delta;
    BitWriter delta_writer(delta);
    delta_writer.WriteBool(false);
    delta_writer.WriteVarInt(10);
    delta_writer.Finish();
    const uint32_t base[2] = {2000, 0};
    BitReader delta_reader(delta);
    TESSERA_CHECK(!ReadDelta(d, delta_reader, base, back));
    TESSERA_CHECK(back[0] == 7 && back[1] == 7);
}

TESSERA_TEST(bits_property_of_width_64_round_trips) {
    ReplicationSchema schema;
    TESSERA_CHECK(schema.AddType(0, {PropertyDescriptor::Bits(TESSERA_COLUMN(IntColumns, u64), 64)}));
    const PropertyDescriptor& d = schema.Find(0)->properties[0];
    IntColumns columns{};
    columns.u64[2] = 0xFFFFFFFFFFFFFFFFull;
    columns.u64[3] = 0x8000000000000001ull;
    const Words2 all = QuantizeFrom(d, &columns, 2);
    const Words2 edges = QuantizeFrom(d, &columns, 3);
    TESSERA_CHECK(all.w[0] == 0xFFFFFFFFu && all.w[1] == 0xFFFFFFFFu);
    TESSERA_CHECK(edges.w[0] == 1u && edges.w[1] == 0x80000000u);
    std::vector<uint8_t> bytes;
    BitWriter writer(bytes);
    WriteAbsolute(d, edges.w, writer);
    TESSERA_CHECK(writer.BitCount() == 64);
    writer.Finish();
    BitReader reader(bytes);
    uint32_t back[2] = {0, 0};
    TESSERA_CHECK(ReadAbsolute(d, reader, back));
    TESSERA_CHECK(back[0] == edges.w[0] && back[1] == edges.w[1]);
}

TESSERA_TEST(bits_value_is_masked_to_its_width) {
    ReplicationSchema schema;
    TESSERA_CHECK(schema.AddType(0, {PropertyDescriptor::Bits(TESSERA_COLUMN(IntColumns, u16), 12)}));
    IntColumns columns{};
    columns.u16[0] = 0xFFFF;
    const Words2 words = QuantizeFrom(schema.Find(0)->properties[0], &columns, 0);
    TESSERA_CHECK(words.w[0] == 0xFFFu && words.w[1] == 0);
}

TESSERA_TEST(bits_delta_is_the_xor_when_cheaper) {
    ReplicationSchema schema;
    TESSERA_CHECK(schema.AddType(0, {PropertyDescriptor::Bits(TESSERA_COLUMN(IntColumns, u64), 64)}));
    const PropertyDescriptor& d = schema.Find(0)->properties[0];
    IntColumns columns{};
    columns.u64[0] = 0xF0F0F0F0F0F0F0F0ull;
    columns.u64[1] = 0xF0F0F0F0F0F0F0F8ull; // one bit flipped: the xor is 8
    columns.u64[2] = 0x0F0F0F0F0F0F0F0Full; // every bit flipped
    const Words2 base = QuantizeFrom(d, &columns, 0);
    const Words2 one = QuantizeFrom(d, &columns, 1);
    const Words2 all = QuantizeFrom(d, &columns, 2);
    uint64_t bits = 0;
    std::vector<uint8_t> bytes = WriteDeltaBytes(d, base.w, one.w, &bits);
    TESSERA_CHECK(bits == 1 + VarUIntBits(8)); // the xor, not 64 bits
    BitReader reader(bytes);
    uint32_t back[2] = {0, 0};
    TESSERA_CHECK(ReadDelta(d, reader, base.w, back));
    TESSERA_CHECK(back[0] == one.w[0] && back[1] == one.w[1]);
    bytes = WriteDeltaBytes(d, base.w, all.w, &bits);
    TESSERA_CHECK(bits == 1 + 64); // the xor is as large as the value, so the full value is written
    BitReader all_reader(bytes);
    TESSERA_CHECK(ReadDelta(d, all_reader, base.w, back));
    TESSERA_CHECK(back[0] == all.w[0] && back[1] == all.w[1]);
}

TESSERA_TEST(bits_property_round_trips_every_single_flag) {
    const ReplicationSchema schema = MakeTestSchema();
    const PropertyDescriptor& d = FlagsOf(schema); // 16 flags
    for (uint32_t flag = 0; flag < 16; ++flag) {
        const uint32_t base[2] = {0xA5A5u, 0};
        const uint32_t cur[2] = {0xA5A5u ^ (1u << flag), 0};
        const std::vector<uint8_t> bytes = WriteDeltaBytes(d, base, cur, nullptr);
        BitReader reader(bytes);
        uint32_t back[2] = {0, 0};
        TESSERA_CHECK(ReadDelta(d, reader, base, back));
        TESSERA_CHECK(back[0] == cur[0] && back[1] == 0);
    }
}

TESSERA_TEST(enum_is_always_absolute_with_fixed_width) {
    const ReplicationSchema schema = MakeTestSchema();
    const PropertyDescriptor& d = StateOf(schema); // 8 values: 3 bits
    const uint32_t base = 2, cur = 5;
    uint64_t bits = 0;
    const std::vector<uint8_t> bytes = WriteDeltaBytes(d, &base, &cur, &bits);
    TESSERA_CHECK(bits == 3); // no flag bit
    BitReader reader(bytes);
    uint32_t back = 0;
    TESSERA_CHECK(ReadDelta(d, reader, &base, &back));
    TESSERA_CHECK(back == 5);
    TESSERA_CHECK(AbsoluteBits(d, &cur) == 3);
}

TESSERA_TEST(enum_out_of_range_is_limited_to_the_last_value) {
    const ReplicationSchema schema = MakeTestSchema();
    auto columns = std::make_unique<TestColumns>();
    columns->state[4] = 200;
    uint32_t word = 99;
    QuantizeProperty(StateOf(schema), reinterpret_cast<const std::byte*>(columns.get()), 4, &word);
    TESSERA_CHECK(word == 7);
    columns->state[4] = 3;
    QuantizeProperty(StateOf(schema), reinterpret_cast<const std::byte*>(columns.get()), 4, &word);
    TESSERA_CHECK(word == 3);
}

TESSERA_TEST(enum_read_rejects_a_value_at_or_above_count) {
    ReplicationSchema schema;
    TESSERA_CHECK(schema.AddType(0, {PropertyDescriptor::Enum(TESSERA_COLUMN(IntColumns, u8), 5)}));
    const PropertyDescriptor& d = schema.Find(0)->properties[0];
    std::vector<uint8_t> bytes;
    BitWriter writer(bytes);
    writer.WriteBits(5, 3); // 3 bits hold 5, but the values are 0 to 4
    writer.Finish();
    BitReader reader(bytes);
    uint32_t back = 77;
    TESSERA_CHECK(!ReadAbsolute(d, reader, &back));
    TESSERA_CHECK(back == 77);
}
