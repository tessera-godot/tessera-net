// SPDX-License-Identifier: Apache-2.0
#include "hand_written_example.hpp"
#include "schema_test_support.hpp"
#include "tessera/schema/columns.hpp"
#include "tessera/schema/schema_check.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace tessera::schema;
using schema_test::HandWrittenSchema;
using schema_test::Has;
using schema_test::ReadTestFile;

namespace {

SchemaFile ReadFixture(const std::string& name) {
    const ReadResult read = ReadSchema(ReadTestFile("fixtures/" + name));
    TESSERA_CHECK(read.file.has_value());
    return read.file ? *read.file : SchemaFile{};
}

PropertyDecl Prop(const std::string& name, Kind kind) {
    PropertyDecl p;
    p.name = name;
    p.kind = kind;
    return p;
}

PropertyDecl Position(const std::string& name, double step, double range) {
    PropertyDecl p = Prop(name, Kind::Position);
    p.step = step;
    p.range = range;
    return p;
}

PropertyDecl Angle(const std::string& name, uint32_t bits) {
    PropertyDecl p = Prop(name, Kind::Angle);
    p.bits = bits;
    return p;
}

PropertyDecl Mask(const std::string& name, uint32_t bits) {
    PropertyDecl p = Prop(name, Kind::Mask);
    p.bits = bits;
    return p;
}

PropertyDecl Choice(const std::string& name, uint32_t count) {
    PropertyDecl p = Prop(name, Kind::Choice);
    p.count = count;
    return p;
}

PropertyDecl Integer(const std::string& name, int64_t min, int64_t max, bool is_signed) {
    PropertyDecl p = Prop(name, Kind::Integer);
    p.min_value = min;
    p.max_value = max;
    p.is_signed = is_signed;
    return p;
}

// A file with one type, 1 "entity", and these properties. Each property gets a place of its own (line 10 +
// its number), so that a diagnostic can be traced back to it.
SchemaFile OneTypeFile(std::vector<PropertyDecl> properties) {
    SchemaFile file;
    file.format_version = 1;
    TypeDecl type;
    type.type = 1;
    type.name = "entity";
    type.where.position = {5, 3};
    for (size_t i = 0; i < properties.size(); ++i) {
        properties[i].where.position = {static_cast<uint32_t>(10 + i), 7};
    }
    type.properties = std::move(properties);
    file.types.push_back(std::move(type));
    return file;
}

bool Accepts(const PropertyDecl& property) {
    return Check(OneTypeFile({property})).schema != nullptr;
}

// A mixed type: one property of each kind.
SchemaFile Mixed() {
    return OneTypeFile({Position("pos", 0.01, 2048.0), Angle("yaw", 12), Integer("hp", 0, 100, false),
                        Prop("alive", Kind::Flag), Mask("flags", 8), Choice("team", 5)});
}

uint64_t HashOf(const SchemaFile& file) {
    const CheckResult result = Check(file);
    return result.schema ? result.hash : 0;
}

} // namespace

TESSERA_TEST(check_builds_the_schema_the_core_builds) {
    const SchemaFile file = ReadFixture("all_kinds.json");
    const CheckResult result = Check(file);
    TESSERA_CHECK(result.diagnostics.empty());
    TESSERA_CHECK(result.schema != nullptr);
    if (!result.schema) {
        return;
    }
    TESSERA_CHECK(result.hash == result.schema->Hash());
    const tessera::net::TypeSchema* type = result.schema->Find(1);
    TESSERA_CHECK(type != nullptr && type->properties.size() == 16 && type->words == 30);
    TESSERA_CHECK(result.schema->Find(255) != nullptr && result.schema->Find(255)->words == 2);
    TESSERA_CHECK(result.schema->Find(0) == nullptr && result.schema->Find(2) == nullptr);
    TESSERA_CHECK(result.schema->MaxWords() == 30);
    if (type == nullptr || type->properties.size() != 16) {
        return;
    }
    using tessera::net::PropertyKind;
    const auto& p = type->properties;
    TESSERA_CHECK(p[0].kind == PropertyKind::Position && p[0].step == 0.01f && p[0].range == 2048.0f);
    TESSERA_CHECK(p[1].kind == PropertyKind::Angle && p[1].bits == 12);
    TESSERA_CHECK(p[2].kind == PropertyKind::Integer && !p[2].is_signed && p[2].min_value == 0 &&
                  p[2].max_value == 255);
    TESSERA_CHECK(p[5].kind == PropertyKind::Integer && p[5].max_value == 4294967296ll);
    TESSERA_CHECK(p[6].is_signed && p[6].min_value == -128 && p[6].max_value == 127);
    TESSERA_CHECK(p[9].is_signed && p[9].min_value == -2147483649ll && p[9].max_value == 0);
    TESSERA_CHECK(p[10].is_signed && p[10].min_value == INT64_MIN && p[10].max_value == INT64_MAX);
    TESSERA_CHECK(p[11].kind == PropertyKind::Bits && p[11].bits == 1);
    TESSERA_CHECK(p[12].kind == PropertyKind::Bits && p[12].bits == 8);
    TESSERA_CHECK(p[13].kind == PropertyKind::Bits && p[13].bits == 40);
    TESSERA_CHECK(p[14].kind == PropertyKind::Enum && p[14].enum_count == 5);
    TESSERA_CHECK(p[15].kind == PropertyKind::Enum && p[15].enum_count == 300);
}

TESSERA_TEST(check_hash_equals_hand_written_example_schema_with_players_and_without) {
    SchemaFile file = ReadFixture("example.json");
    TESSERA_CHECK(file.types.size() == 2);
    TESSERA_CHECK(HashOf(file) == schema_test::kHandWrittenHashWithPlayers);
    TESSERA_CHECK(HashOf(file) == HandWrittenSchema(true).Hash());
    file.types.pop_back(); // without the players
    TESSERA_CHECK(HashOf(file) == schema_test::kHandWrittenHashWithoutPlayers);
    TESSERA_CHECK(HashOf(file) == HandWrittenSchema(false).Hash());
}

TESSERA_TEST(check_hash_does_not_depend_on_the_order_of_the_types_in_the_file) {
    SchemaFile file = ReadFixture("all_kinds.json");
    const uint64_t hash = HashOf(file);
    TESSERA_CHECK(hash != 0);
    std::reverse(file.types.begin(), file.types.end());
    TESSERA_CHECK(HashOf(file) == hash);
}

TESSERA_TEST(check_hash_follows_every_parameter) {
    const uint64_t base = HashOf(Mixed());
    TESSERA_CHECK(base != 0);
    const auto changed = [&](auto change) {
        SchemaFile file = Mixed();
        change(file.types[0]);
        const uint64_t hash = HashOf(file);
        return hash != 0 && hash != base;
    };
    TESSERA_CHECK(changed([](TypeDecl& t) { t.properties[0].step = 0.02; }));
    TESSERA_CHECK(changed([](TypeDecl& t) { t.properties[0].range = 1024.0; }));
    TESSERA_CHECK(changed([](TypeDecl& t) { t.properties[1].bits = 11; }));
    TESSERA_CHECK(changed([](TypeDecl& t) { t.properties[2].min_value = 1; }));
    TESSERA_CHECK(changed([](TypeDecl& t) { t.properties[2].max_value = 101; }));
    TESSERA_CHECK(changed([](TypeDecl& t) { t.properties[2].is_signed = true; }));
    TESSERA_CHECK(changed([](TypeDecl& t) { t.properties[4].bits = 9; }));
    TESSERA_CHECK(changed([](TypeDecl& t) { t.properties[5].count = 6; }));
    TESSERA_CHECK(changed([](TypeDecl& t) { t.type = 2; }));
    TESSERA_CHECK(changed([](TypeDecl& t) { std::swap(t.properties[1], t.properties[5]); }));
    TESSERA_CHECK(changed([](TypeDecl& t) { t.properties.pop_back(); }));
    // A flag is a mask of one bit, on the wire and in the hash: that is not a parameter that changes
    // anything.
    TESSERA_CHECK(!changed([](TypeDecl& t) { t.properties[3] = Mask("alive", 1); }));
}

TESSERA_TEST(check_passes_the_core_message_for_a_bad_property_with_file_position) {
    const CheckResult result = Check(OneTypeFile({Prop("alive", Kind::Flag), Position("pos", 0.0, 100.0)}));
    TESSERA_CHECK(result.schema == nullptr && result.diagnostics.size() == 1);
    if (result.diagnostics.size() != 1) {
        return;
    }
    const Diagnostic& d = result.diagnostics[0];
    TESSERA_CHECK(d.position.line == 11 && d.position.column == 7); // the second property
    TESSERA_CHECK(d.message ==
                  "type \"entity\" (1): property 1: position step must be a positive, finite number");
    TESSERA_CHECK(Has(d.fix, "\"step\" and \"range\" are numbers above 0"));
}

TESSERA_TEST(check_points_at_the_first_property_the_core_refuses) {
    // Two bad ones: the check points at the first.
    const CheckResult result =
        Check(OneTypeFile({Prop("alive", Kind::Flag), Angle("yaw", 0), Mask("flags", 0), Choice("team", 1)}));
    TESSERA_CHECK(result.diagnostics.size() == 1);
    if (!result.diagnostics.empty()) {
        TESSERA_CHECK(result.diagnostics[0].position.line == 11);
        TESSERA_CHECK(Has(result.diagnostics[0].message, "property 1: angle bits must be 1 to 16"));
    }
    // One bad type does not hide another.
    SchemaFile two = OneTypeFile({Angle("yaw", 0)});
    TypeDecl second = two.types[0];
    second.type = 2;
    second.name = "other";
    second.properties = {Mask("flags", 65)};
    second.properties[0].where.position = {40, 2};
    two.types.push_back(second);
    const CheckResult both = Check(two);
    TESSERA_CHECK(both.diagnostics.size() == 2);
    if (both.diagnostics.size() == 2) {
        TESSERA_CHECK(both.diagnostics[0].position.line == 10 && both.diagnostics[1].position.line == 40);
        TESSERA_CHECK(Has(both.diagnostics[1].message, "type \"other\" (2): property 0:"));
    }
}

TESSERA_TEST(check_points_at_a_property_that_is_bad_by_itself_before_the_word_limit) {
    // 20 flags are too many (2 words each, the limit is 32), and the 19th is also a mask of 65 bits. The
    // property that is wrong on its own is the one to show: the limit of words only matters once the
    // properties are right.
    std::vector<PropertyDecl> properties;
    for (int i = 0; i < 20; ++i) {
        properties.push_back(i == 18 ? Mask("wide", 65) : Prop("f" + std::to_string(i), Kind::Flag));
    }
    const CheckResult result = Check(OneTypeFile(properties));
    TESSERA_CHECK(result.schema == nullptr && result.diagnostics.size() == 1);
    if (!result.diagnostics.empty()) {
        TESSERA_CHECK(result.diagnostics[0].position.line == 10 + 18);
        TESSERA_CHECK(Has(result.diagnostics[0].fix, "from 1 to 64"));
    }
    // Without the bad one, the word limit is the problem, and it is the 17th that passes it.
    properties[18] = Prop("f18", Kind::Flag);
    const CheckResult limit = Check(OneTypeFile(properties));
    TESSERA_CHECK(limit.diagnostics.size() == 1 && limit.diagnostics[0].position.line == 10 + 16);
    TESSERA_CHECK(!limit.diagnostics.empty() && Has(limit.diagnostics[0].fix, "at most 32 words"));
}

TESSERA_TEST(check_refuses_what_add_type_refuses_boundaries) {
    // Each limit is accepted on one side and refused on the other, and the text of the correction names the
    // limit that was just seen.
    const auto fix_of = [](const PropertyDecl& p) {
        const CheckResult result = Check(OneTypeFile({p}));
        return result.diagnostics.empty() ? std::string() : result.diagnostics[0].fix;
    };
    // angle: 1 to 16
    TESSERA_CHECK(!Accepts(Angle("a", 0)) && Accepts(Angle("a", 1)) && Accepts(Angle("a", 16)) &&
                  !Accepts(Angle("a", 17)));
    TESSERA_CHECK(Has(fix_of(Angle("a", 0)), "from 1 to 16") && Has(fix_of(Angle("a", 17)), "from 1 to 16"));
    // mask: 1 to 64 (the column grows with the bits asked for, up to 64)
    TESSERA_CHECK(!Accepts(Mask("m", 0)) && Accepts(Mask("m", 1)) && Accepts(Mask("m", 64)) &&
                  !Accepts(Mask("m", 65)));
    TESSERA_CHECK(Has(fix_of(Mask("m", 0)), "from 1 to 64") && Has(fix_of(Mask("m", 65)), "from 1 to 64"));
    // choice: at least 2 values
    TESSERA_CHECK(!Accepts(Choice("c", 0)) && !Accepts(Choice("c", 1)) && Accepts(Choice("c", 2)));
    TESSERA_CHECK(Has(fix_of(Choice("c", 1)), "from 2"));
    // position: step and range above 0, and at most 2^31 - 1 steps from -range to range
    TESSERA_CHECK(!Accepts(Position("p", 0.0, 10.0)) && !Accepts(Position("p", -1.0, 10.0)));
    TESSERA_CHECK(!Accepts(Position("p", 1.0, 0.0)) && !Accepts(Position("p", 1.0, -5.0)));
    TESSERA_CHECK(Accepts(Position("p", 2.0, 2147483520.0)) && !Accepts(Position("p", 2.0, 2147483648.0)));
    TESSERA_CHECK(Has(fix_of(Position("p", 2.0, 2147483648.0)), "at most 2147483647"));
    // integer: min not above max, unsigned never below 0, and the range fits the column
    TESSERA_CHECK(!Accepts(Integer("i", 5, 4, false)) && Accepts(Integer("i", 5, 5, false)));
    TESSERA_CHECK(!Accepts(Integer("i", -1, 5, false)) && Accepts(Integer("i", -1, 5, true)));
    TESSERA_CHECK(Accepts(Integer("i", 0, 255, false)) && Accepts(Integer("i", 0, 256, false)));
    TESSERA_CHECK(Accepts(Integer("i", -128, 127, true)) && Accepts(Integer("i", -129, 127, true)));
    TESSERA_CHECK(Accepts(
        Integer("i", std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(), true)));
    TESSERA_CHECK(Accepts(Integer("i", 0, std::numeric_limits<int64_t>::max(), false)));
    TESSERA_CHECK(Has(fix_of(Integer("i", 5, 4, false)), "\"min\" must not be above \"max\""));
}

TESSERA_TEST(check_more_than_64_properties) {
    std::vector<PropertyDecl> properties;
    for (int i = 0; i < 65; ++i) {
        properties.push_back(Angle("a" + std::to_string(i), 8));
    }
    const CheckResult result = Check(OneTypeFile(properties));
    TESSERA_CHECK(result.schema == nullptr && result.diagnostics.size() == 1);
    if (result.diagnostics.empty()) {
        return;
    }
    // The core says "at most 64"; but 32 words is the lower limit (an angle takes one), so the check points
    // at the property that passes it, the 33rd, which is the one to remove.
    TESSERA_CHECK(Has(result.diagnostics[0].message, "a type can have at most 64 properties"));
    TESSERA_CHECK(result.diagnostics[0].position.line == 10 + 32);
    TESSERA_CHECK(Has(result.diagnostics[0].fix, "at most 32 words"));
    // 32 of them fit exactly.
    properties.resize(32);
    TESSERA_CHECK(Check(OneTypeFile(properties)).schema != nullptr);
}

TESSERA_TEST(check_more_than_32_words) {
    std::vector<PropertyDecl> properties;
    for (int i = 0; i < 11; ++i) { // 3 words each: 33
        properties.push_back(Position("p" + std::to_string(i), 0.01, 100.0));
    }
    const CheckResult result = Check(OneTypeFile(properties));
    TESSERA_CHECK(result.schema == nullptr && result.diagnostics.size() == 1);
    if (!result.diagnostics.empty()) {
        TESSERA_CHECK(result.diagnostics[0].position.line == 10 + 10); // the 11th takes it past 32
        TESSERA_CHECK(Has(result.diagnostics[0].message, "takes 33 words, the limit is 32"));
        TESSERA_CHECK(Has(result.diagnostics[0].fix, "at most 32 words"));
    }
    properties.pop_back();                              // 30 words
    properties.push_back(Integer("hp", 0, 100, false)); // 32 words exactly
    TESSERA_CHECK(Check(OneTypeFile(properties)).schema != nullptr);
    properties.push_back(Angle("yaw", 8));
    TESSERA_CHECK(Check(OneTypeFile(properties)).schema == nullptr);
}

TESSERA_TEST(check_float_numbers_are_converted_like_the_core_expects) {
    // A step too small for a float becomes zero, and a range too large becomes infinite: the core refuses
    // both.
    CheckResult tiny = Check(OneTypeFile({Position("p", 1e-50, 100.0)}));
    TESSERA_CHECK(tiny.schema == nullptr && tiny.diagnostics.size() == 1);
    TESSERA_CHECK(!tiny.diagnostics.empty() &&
                  Has(tiny.diagnostics[0].message, "position step must be a positive, finite number"));
    CheckResult huge = Check(OneTypeFile({Position("p", 0.01, 1e50)}));
    TESSERA_CHECK(huge.schema == nullptr && huge.diagnostics.size() == 1);
    TESSERA_CHECK(!huge.diagnostics.empty() &&
                  Has(huge.diagnostics[0].message, "position range must be a positive, finite number"));
    CheckResult negative_huge = Check(OneTypeFile({Position("p", -1e300, 100.0)}));
    TESSERA_CHECK(negative_huge.schema == nullptr);
    // 0.01 is the float 0.01f, and the hash is the one the core gives for that float.
    const CheckResult fine = Check(OneTypeFile({Position("p", 0.01, 2048.0)}));
    TESSERA_CHECK(fine.schema != nullptr);
    tessera::net::ReplicationSchema by_hand;
    by_hand.AddType(1, {tessera::net::PropertyDescriptor::Position(
                           tessera::net::ColumnRef{0, 4}, tessera::net::ColumnRef{0, 4},
                           tessera::net::ColumnRef{0, 4}, 0.01f, 2048.0f)});
    TESSERA_CHECK(fine.hash == by_hand.Hash());
}

TESSERA_TEST(check_a_failed_check_makes_no_schema) {
    const CheckResult result = Check(OneTypeFile({Angle("yaw", 0)}));
    TESSERA_CHECK(result.schema == nullptr);
    TESSERA_CHECK(!result.diagnostics.empty());
    TESSERA_CHECK(result.hash == 0);
    // A type with no property is refused too, where the type is.
    const CheckResult empty = Check(OneTypeFile({}));
    TESSERA_CHECK(empty.schema == nullptr && empty.diagnostics.size() == 1);
    if (!empty.diagnostics.empty()) {
        TESSERA_CHECK(empty.diagnostics[0].position.line == 5 && empty.diagnostics[0].position.column == 3);
        TESSERA_CHECK(Has(empty.diagnostics[0].message, "needs at least one property"));
    }
}
