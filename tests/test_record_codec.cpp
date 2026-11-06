// SPDX-License-Identifier: Apache-2.0
#include "replication_test_support.hpp"
#include "tessera/net/replication/record_codec.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace tessera::net;
using namespace replication_test;

namespace {

// Nine words of the mob type: position 0..2, angle 3, health 4..5, flags 6..7, state 8.
struct Words {
    uint32_t w[9] = {1'000'000, 1'000'000, 1'000'000, 100, 1000, 0, 0xA5, 0, 2};
};

} // namespace

TESSERA_TEST(terminator_is_a_single_zero_gap) {
    std::vector<uint8_t> bytes;
    BitWriter writer(bytes);
    WriteTerminator(writer);
    TESSERA_CHECK(writer.BitCount() == 4);
    writer.Finish();
    BitReader reader(bytes);
    uint32_t gap = 99, age = 99;
    bool complete = true;
    TESSERA_CHECK(ReadRecordHead(reader, &gap, &complete, &age) && gap == 0);
}

TESSERA_TEST(complete_record_has_no_age_and_no_mask) {
    const ReplicationSchema schema = MakeTestSchema();
    const TypeSchema& type = *schema.Find(kMobType);
    const Words current;
    std::vector<uint8_t> bytes;
    BitWriter writer(bytes);
    WriteRecord(type, 1, 0, nullptr, current.w, writer);
    // gap 4 bits, the flag 1, position 3 x 21, angle 10, health 11, flags 16, state 3
    TESSERA_CHECK(writer.BitCount() == 4 + 1 + 63 + 10 + 11 + 16 + 3);
    writer.Finish();
    BitReader reader(bytes);
    uint32_t gap = 0, age = 7;
    bool complete = false;
    TESSERA_CHECK(ReadRecordHead(reader, &gap, &complete, &age) && gap == 1 && complete);
    uint32_t back[9] = {};
    TESSERA_CHECK(ReadRecordBody(type, reader, true, nullptr, back));
    TESSERA_CHECK(std::memcmp(back, current.w, sizeof(back)) == 0);
}

TESSERA_TEST(partial_record_round_trips_every_single_property_change) {
    const ReplicationSchema schema = MakeTestSchema();
    const TypeSchema& type = *schema.Find(kMobType);
    const Words base;
    for (uint32_t property = 0; property < 5; ++property) {
        Words current = base;
        const PropertyDescriptor& d = type.properties[property];
        current.w[d.word_offset] += 1; // the first word of that property
        std::vector<uint8_t> bytes;
        BitWriter writer(bytes);
        WriteRecord(type, 3, 2, base.w, current.w, writer);
        writer.Finish();
        BitReader reader(bytes);
        uint32_t gap = 0, age = 0;
        bool complete = true;
        TESSERA_CHECK(ReadRecordHead(reader, &gap, &complete, &age));
        TESSERA_CHECK(gap == 3 && !complete && age == 2);
        uint32_t back[9] = {};
        TESSERA_CHECK(ReadRecordBody(type, reader, false, base.w, back));
        TESSERA_CHECK(std::memcmp(back, current.w, sizeof(back)) == 0);
    }
}

TESSERA_TEST(partial_record_with_nothing_changed_has_an_empty_mask) {
    const ReplicationSchema schema = MakeTestSchema();
    const TypeSchema& type = *schema.Find(kMobType);
    const Words same;
    std::vector<uint8_t> bytes;
    BitWriter writer(bytes);
    WriteRecord(type, 1, 1, same.w, same.w, writer);
    TESSERA_CHECK(writer.BitCount() == 4 + 1 + 4 + 5); // gap, flag, age 1 as 0, a 5-bit mask of zeros
    writer.Finish();
    BitReader reader(bytes);
    uint32_t gap = 0, age = 0;
    bool complete = true;
    TESSERA_CHECK(ReadRecordHead(reader, &gap, &complete, &age));
    uint32_t back[9] = {};
    TESSERA_CHECK(ReadRecordBody(type, reader, false, same.w, back));
    TESSERA_CHECK(std::memcmp(back, same.w, sizeof(back)) == 0);
}

TESSERA_TEST(record_body_rejects_a_value_outside_its_range_and_leaves_the_output_alone) {
    const ReplicationSchema schema = MakeTestSchema();
    const TypeSchema& type = *schema.Find(kMobType);
    std::vector<uint8_t> bytes;
    BitWriter writer(bytes);
    writer.WriteBits(2'000'001, 21); // a position past its largest step
    writer.WriteBits(0, 21);
    writer.WriteBits(0, 21);
    writer.Finish();
    BitReader reader(bytes);
    uint32_t back[9];
    for (uint32_t& w : back) {
        w = 0x77777777;
    }
    TESSERA_CHECK(!ReadRecordBody(type, reader, true, nullptr, back));
    for (const uint32_t w : back) {
        TESSERA_CHECK(w == 0x77777777);
    }
}

TESSERA_TEST(record_head_fails_on_a_truncated_gap) {
    std::vector<uint8_t> bytes;
    BitReader reader(bytes);
    uint32_t gap = 5, age = 5;
    bool complete = true;
    TESSERA_CHECK(!ReadRecordHead(reader, &gap, &complete, &age));
}
