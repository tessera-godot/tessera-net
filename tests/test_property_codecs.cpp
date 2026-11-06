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
