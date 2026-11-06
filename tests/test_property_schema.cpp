// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/replication/property_schema.hpp"
#include "test_support.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace tessera::net;

namespace {

constexpr ColumnRef kFloat{0, 4};
constexpr ColumnRef kInt32{16, 4};
constexpr ColumnRef kU16{32, 2};
constexpr ColumnRef kU8{48, 1};

PropertyDescriptor Pos() {
    return PropertyDescriptor::Position(ColumnRef{0, 4}, ColumnRef{4, 4}, ColumnRef{8, 4}, 0.01f, 10000.0f);
}

std::vector<PropertyDescriptor> FullType() {
    return {Pos(), PropertyDescriptor::Angle(kFloat, 10),
            PropertyDescriptor::Integer(kInt32, true, -1000, 1000), PropertyDescriptor::Bits(kU16, 16),
            PropertyDescriptor::Enum(kU8, 8)};
}

bool Rejects(std::vector<PropertyDescriptor> properties, uint32_t type = 0) {
    ReplicationSchema schema;
    std::string error;
    const bool accepted = schema.AddType(type, std::move(properties), &error);
    return !accepted && !error.empty() && schema.Find(type) == nullptr;
}

uint64_t HashOf(std::vector<PropertyDescriptor> properties, uint32_t type = 0) {
    ReplicationSchema schema;
    TESSERA_CHECK(schema.AddType(type, std::move(properties)));
    return schema.Hash();
}

} // namespace

TESSERA_TEST(descriptor_factories_set_the_kind_and_parameters) {
    const PropertyDescriptor position = Pos();
    TESSERA_CHECK(position.kind == PropertyKind::Position);
    TESSERA_CHECK(position.source[0].offset == 0 && position.source[1].offset == 4 &&
                  position.source[2].offset == 8);
    TESSERA_CHECK(position.step == 0.01f && position.range == 10000.0f);
    const PropertyDescriptor angle = PropertyDescriptor::Angle(kFloat, 12);
    TESSERA_CHECK(angle.kind == PropertyKind::Angle && angle.bits == 12);
    const PropertyDescriptor integer = PropertyDescriptor::Integer(kInt32, true, -5, 9);
    TESSERA_CHECK(integer.kind == PropertyKind::Integer && integer.is_signed && integer.min_value == -5 &&
                  integer.max_value == 9);
    const PropertyDescriptor bits = PropertyDescriptor::Bits(kU16, 11);
    TESSERA_CHECK(bits.kind == PropertyKind::Bits && bits.bits == 11);
    const PropertyDescriptor choice = PropertyDescriptor::Enum(kU8, 5);
    TESSERA_CHECK(choice.kind == PropertyKind::Enum && choice.enum_count == 5);
}

TESSERA_TEST(add_type_computes_word_offsets_and_total_words) {
    ReplicationSchema schema;
    TESSERA_CHECK(schema.AddType(3, FullType()));
    const TypeSchema* type = schema.Find(3);
    TESSERA_CHECK(type != nullptr && type->type == 3);
    TESSERA_CHECK(type->words == 9);
    TESSERA_CHECK(type->properties.size() == 5);
    const uint32_t offsets[] = {0, 3, 4, 6, 8};
    const uint32_t words[] = {3, 1, 2, 2, 1};
    for (size_t i = 0; i < 5; ++i) {
        TESSERA_CHECK(type->properties[i].word_offset == offsets[i]);
        TESSERA_CHECK(type->properties[i].words == words[i]);
    }
    TESSERA_CHECK(schema.MaxWords() == 9);
}

TESSERA_TEST(position_ordinals_follow_range_and_step) {
    ReplicationSchema schema;
    TESSERA_CHECK(schema.AddType(0, {Pos()}));
    const PropertyDescriptor& position = schema.Find(0)->properties[0];
    TESSERA_CHECK(position.max_ordinal == 2'000'000);
    TESSERA_CHECK(position.absolute_bits == 21);
}

TESSERA_TEST(angle_bits_enum_and_integer_absolute_widths) {
    ReplicationSchema schema;
    TESSERA_CHECK(schema.AddType(0, FullType()));
    const TypeSchema* type = schema.Find(0);
    TESSERA_CHECK(type->properties[1].absolute_bits == 10 && type->properties[1].max_ordinal == 1023);
    TESSERA_CHECK(type->properties[2].absolute_bits == 11 && type->properties[2].max_ordinal == 2000);
    TESSERA_CHECK(type->properties[3].absolute_bits == 16 && type->properties[3].max_ordinal == 0xFFFF);
    TESSERA_CHECK(type->properties[4].absolute_bits == 3 && type->properties[4].max_ordinal == 7);
    TESSERA_CHECK(schema.AddType(1, {PropertyDescriptor::Enum(kU8, 5)}));
    TESSERA_CHECK(schema.Find(1)->properties[0].absolute_bits == 3);
    TESSERA_CHECK(schema.AddType(
        2, {PropertyDescriptor::Integer(ColumnRef{0, 8}, true, std::numeric_limits<int64_t>::min(),
                                        std::numeric_limits<int64_t>::max())}));
    TESSERA_CHECK(schema.Find(2)->properties[0].absolute_bits == 64);
    TESSERA_CHECK(schema.Find(2)->properties[0].max_ordinal == std::numeric_limits<uint64_t>::max());
}

TESSERA_TEST(add_type_rejects_bad_input_with_a_message) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    TESSERA_CHECK(Rejects({}, 0));           // no properties
    TESSERA_CHECK(Rejects(FullType(), 256)); // type above the maximum
    TESSERA_CHECK(
        Rejects(std::vector<PropertyDescriptor>(65, PropertyDescriptor::Enum(kU8, 2)))); // 65 properties
    {
        // 17 positions are 51 words: over the 32-word limit
        std::vector<PropertyDescriptor> too_wide(11, Pos());
        TESSERA_CHECK(Rejects(too_wide));
    }
    TESSERA_CHECK(Rejects({PropertyDescriptor::Position(kFloat, kFloat, kFloat, 0.0f, 100.0f)}));
    TESSERA_CHECK(Rejects({PropertyDescriptor::Position(kFloat, kFloat, kFloat, nan, 100.0f)}));
    TESSERA_CHECK(Rejects({PropertyDescriptor::Position(kFloat, kFloat, kFloat, -0.01f, 100.0f)}));
    TESSERA_CHECK(Rejects({PropertyDescriptor::Position(kFloat, kFloat, kFloat, 0.01f, 0.0f)}));
    TESSERA_CHECK(Rejects({PropertyDescriptor::Position(kFloat, kFloat, kFloat, 0.01f, nan)}));
    TESSERA_CHECK(
        Rejects({PropertyDescriptor::Position(kFloat, kFloat, kFloat, 0.001f, 1.0e7f)})); // ordinal too large
    TESSERA_CHECK(Rejects({PropertyDescriptor::Angle(kFloat, 0)}));
    TESSERA_CHECK(Rejects({PropertyDescriptor::Angle(kFloat, 17)}));
    TESSERA_CHECK(Rejects({PropertyDescriptor::Integer(kInt32, true, 10, 9)}));
    TESSERA_CHECK(Rejects({PropertyDescriptor::Integer(kU8, false, 0, 300)})); // range does not fit one byte
    TESSERA_CHECK(Rejects({PropertyDescriptor::Bits(kU16, 0)}));
    TESSERA_CHECK(Rejects({PropertyDescriptor::Bits(kU16, 65)}));
    TESSERA_CHECK(Rejects({PropertyDescriptor::Bits(kU8, 9)})); // wider than its column
    TESSERA_CHECK(Rejects({PropertyDescriptor::Enum(kU8, 1)}));
    TESSERA_CHECK(Rejects({PropertyDescriptor::Enum(ColumnRef{0, 3}, 4)}));  // 3-byte element
    TESSERA_CHECK(Rejects({PropertyDescriptor::Angle(ColumnRef{0, 8}, 8)})); // angle source must be a float
    {
        ReplicationSchema schema;
        TESSERA_CHECK(schema.AddType(0, FullType()));
        std::string error;
        TESSERA_CHECK(!schema.AddType(0, FullType(), &error)); // repeated type
        TESSERA_CHECK(!error.empty());
    }
}

TESSERA_TEST(failed_add_type_leaves_the_schema_unchanged) {
    ReplicationSchema schema;
    TESSERA_CHECK(schema.AddType(0, FullType()));
    const uint64_t before = schema.Hash();
    TESSERA_CHECK(!schema.AddType(1, {PropertyDescriptor::Angle(kFloat, 0)}));
    TESSERA_CHECK(schema.Find(1) == nullptr);
    TESSERA_CHECK(schema.MaxWords() == 9);
    TESSERA_CHECK(schema.Hash() == before);
}

TESSERA_TEST(find_returns_null_for_unknown_types) {
    ReplicationSchema schema;
    TESSERA_CHECK(schema.Find(0) == nullptr);
    TESSERA_CHECK(schema.Find(255) == nullptr);
    TESSERA_CHECK(schema.Find(256) == nullptr);
    TESSERA_CHECK(schema.AddType(7, FullType()));
    TESSERA_CHECK(schema.Find(7) != nullptr);
    TESSERA_CHECK(schema.Find(6) == nullptr);
}

TESSERA_TEST(max_words_is_the_largest_type) {
    ReplicationSchema schema;
    TESSERA_CHECK(schema.MaxWords() == 0);
    TESSERA_CHECK(schema.AddType(0, {PropertyDescriptor::Enum(kU8, 4)}));
    TESSERA_CHECK(schema.MaxWords() == 1);
    TESSERA_CHECK(schema.AddType(1, FullType()));
    TESSERA_CHECK(schema.MaxWords() == 9);
    TESSERA_CHECK(schema.AddType(2, {PropertyDescriptor::Angle(kFloat, 8)}));
    TESSERA_CHECK(schema.MaxWords() == 9);
}

TESSERA_TEST(hash_is_stable_and_depends_on_every_wire_field) {
    const uint64_t base = HashOf(FullType());
    TESSERA_CHECK(HashOf(FullType()) == base);
    TESSERA_CHECK(base == 0xDBDC7F24EAC309B5ull); // golden value, computed outside the C++ code from the canonical form
    TESSERA_CHECK(HashOf(FullType(), 1) != base); // another type id

    auto changed = [&](auto edit) {
        std::vector<PropertyDescriptor> properties = FullType();
        edit(properties);
        return HashOf(properties) != base;
    };
    TESSERA_CHECK(changed([](auto& p) { p[0].step = 0.02f; }));
    TESSERA_CHECK(changed([](auto& p) { p[0].range = 5000.0f; }));
    TESSERA_CHECK(changed([](auto& p) { p[1].bits = 11; }));
    TESSERA_CHECK(changed([](auto& p) {
        p[2].is_signed = false;
        p[2].min_value = 0;
    }));
    TESSERA_CHECK(changed([](auto& p) { p[2].max_value = 999; }));
    TESSERA_CHECK(changed([](auto& p) { p[3].bits = 15; }));
    TESSERA_CHECK(changed([](auto& p) { p[4].enum_count = 9; }));
    TESSERA_CHECK(changed([](auto& p) { std::swap(p[1], p[3]); })); // order of the properties
    TESSERA_CHECK(changed([](auto& p) { p.pop_back(); }));
    // Where the server reads a value from is local layout, not part of the wire format.
    TESSERA_CHECK(!changed([](auto& p) { p[1].source[0].offset = 999; }));
}
