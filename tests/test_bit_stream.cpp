// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/replication/bit_stream.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <limits>
#include <vector>

using tessera::net::BitReader;
using tessera::net::BitsFor;
using tessera::net::BitWriter;
using tessera::net::VarIntBits;
using tessera::net::VarUIntBits;

namespace {

uint64_t Mask(uint32_t width) {
    return width >= 64 ? ~0ull : ((1ull << width) - 1);
}

} // namespace

TESSERA_TEST(bits_round_trip_for_every_width) {
    for (uint32_t width = 0; width <= 64; ++width) {
        const uint64_t patterns[] = {0ull, Mask(width), 0xAAAAAAAAAAAAAAAAull & Mask(width),
                                     0x5555555555555555ull & Mask(width)};
        std::vector<uint8_t> bytes;
        BitWriter writer(bytes);
        for (const uint64_t value : patterns) {
            writer.WriteBits(value, width);
        }
        TESSERA_CHECK(writer.BitCount() == 4ull * width);
        writer.Finish();
        BitReader reader(bytes);
        for (const uint64_t value : patterns) {
            TESSERA_CHECK(reader.ReadBits(width) == value);
        }
        TESSERA_CHECK(reader.ok());
    }
}

TESSERA_TEST(bool_and_mixed_fields_round_trip_in_order) {
    std::vector<uint8_t> bytes;
    BitWriter writer(bytes);
    writer.WriteBool(true);
    writer.WriteBits(5, 3);
    writer.WriteBool(false);
    writer.WriteVarUInt(1000);
    writer.WriteVarInt(-77);
    writer.WriteBits(0x1234567890ABCDEFull, 64);
    writer.Finish();
    BitReader reader(bytes);
    TESSERA_CHECK(reader.ReadBool());
    TESSERA_CHECK(reader.ReadBits(3) == 5);
    TESSERA_CHECK(!reader.ReadBool());
    TESSERA_CHECK(reader.ReadVarUInt() == 1000);
    TESSERA_CHECK(reader.ReadVarInt() == -77);
    TESSERA_CHECK(reader.ReadBits(64) == 0x1234567890ABCDEFull);
    TESSERA_CHECK(reader.ok());
}

TESSERA_TEST(var_uint_round_trip_and_cost) {
    const uint64_t values[] = {0, 7, 8, 63, 64, 511, 512, 1ull << 32, std::numeric_limits<uint64_t>::max()};
    const uint32_t first_costs[] = {4, 4, 8, 8, 12, 12};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        std::vector<uint8_t> bytes;
        BitWriter writer(bytes);
        writer.WriteVarUInt(values[i]);
        TESSERA_CHECK(writer.BitCount() == VarUIntBits(values[i]));
        if (i < 6) {
            TESSERA_CHECK(VarUIntBits(values[i]) == first_costs[i]);
        }
        writer.Finish();
        BitReader reader(bytes);
        TESSERA_CHECK(reader.ReadVarUInt() == values[i]);
        TESSERA_CHECK(reader.ok());
    }
}

TESSERA_TEST(var_int_zigzag_round_trip) {
    const int64_t values[] = {
        0, -1,       1,      std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(), -4,
        4, -1000000, 1000000};
    for (const int64_t value : values) {
        std::vector<uint8_t> bytes;
        BitWriter writer(bytes);
        writer.WriteVarInt(value);
        TESSERA_CHECK(writer.BitCount() == VarIntBits(value));
        writer.Finish();
        BitReader reader(bytes);
        TESSERA_CHECK(reader.ReadVarInt() == value);
        TESSERA_CHECK(reader.ok());
    }
}

TESSERA_TEST(bits_for_matches_the_definition) {
    TESSERA_CHECK(BitsFor(0) == 0);
    TESSERA_CHECK(BitsFor(1) == 1);
    TESSERA_CHECK(BitsFor(2) == 2);
    TESSERA_CHECK(BitsFor(255) == 8);
    TESSERA_CHECK(BitsFor(256) == 9);
    TESSERA_CHECK(BitsFor(std::numeric_limits<uint64_t>::max()) == 64);
}

TESSERA_TEST(reading_past_the_end_sets_the_error_and_returns_zero) {
    const std::vector<uint8_t> bytes = {0xFF};
    BitReader reader(bytes);
    TESSERA_CHECK(reader.ReadBits(8) == 0xFF);
    TESSERA_CHECK(reader.ok());
    TESSERA_CHECK(reader.ReadBits(1) == 0);
    TESSERA_CHECK(!reader.ok());
    TESSERA_CHECK(reader.ReadBits(8) == 0); // later reads stay at zero
    TESSERA_CHECK(!reader.ok());
}

TESSERA_TEST(overlong_var_uint_is_rejected) {
    // 23 chunks that all say "more follows" cannot be a valid 64-bit value.
    std::vector<uint8_t> bytes;
    BitWriter writer(bytes);
    for (int i = 0; i < 23; ++i) {
        writer.WriteBits(0xF, 4);
    }
    writer.WriteBits(0, 4);
    writer.Finish();
    BitReader reader(bytes);
    reader.ReadVarUInt();
    TESSERA_CHECK(!reader.ok());
}

TESSERA_TEST(writer_appends_after_existing_bytes_and_counts_only_its_bits) {
    std::vector<uint8_t> bytes = {0xAB, 0xCD, 0xEF};
    BitWriter writer(bytes);
    writer.WriteBits(0x3, 2);
    TESSERA_CHECK(writer.BitCount() == 2);
    writer.Finish();
    TESSERA_CHECK(bytes.size() == 4);
    TESSERA_CHECK(bytes[0] == 0xAB && bytes[1] == 0xCD && bytes[2] == 0xEF);
    TESSERA_CHECK(bytes[3] == 0x03);
}

TESSERA_TEST(finish_pads_with_zero_bits_to_a_whole_byte) {
    std::vector<uint8_t> bytes;
    BitWriter writer(bytes);
    writer.WriteBits(0x1, 1);
    writer.WriteBits(0x7, 3);
    writer.Finish();
    TESSERA_CHECK(bytes.size() == 1);
    TESSERA_CHECK(bytes[0] == 0x0F); // bit 0 = 1, bits 1..3 = 111, padding bits 4..7 = 0
    std::vector<uint8_t> exact;
    BitWriter full(exact);
    full.WriteBits(0xFF, 8);
    full.Finish();
    TESSERA_CHECK(exact.size() == 1);
}
