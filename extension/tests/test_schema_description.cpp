// SPDX-License-Identifier: Apache-2.0
#include "example_game.hpp"
#include "tessera/ext/schema_description.hpp"
#include "test_support.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace tessera::ext;

namespace {

PropertyDescription Position(const std::string& name, double step = 0.01, double range = 2048.0) {
    PropertyDescription p;
    p.name = name;
    p.kind = "position";
    p.step = step;
    p.range = range;
    return p;
}
PropertyDescription Angle(const std::string& name, uint32_t bits = 12) {
    PropertyDescription p;
    p.name = name;
    p.kind = "angle";
    p.bits = bits;
    return p;
}
PropertyDescription Integer(const std::string& name, int64_t min_value, int64_t max_value, bool is_signed) {
    PropertyDescription p;
    p.name = name;
    p.kind = "integer";
    p.min_value = min_value;
    p.max_value = max_value;
    p.is_signed = is_signed;
    return p;
}
PropertyDescription Flag(const std::string& name) {
    PropertyDescription p;
    p.name = name;
    p.kind = "flag";
    return p;
}
PropertyDescription Mask(const std::string& name, uint32_t bits) {
    PropertyDescription p;
    p.name = name;
    p.kind = "mask";
    p.bits = bits;
    return p;
}
PropertyDescription Choice(const std::string& name, uint32_t count) {
    PropertyDescription p;
    p.name = name;
    p.kind = "choice";
    p.count = count;
    return p;
}

TypeDescription Type(uint32_t type, std::vector<PropertyDescription> properties) {
    return TypeDescription{type, std::move(properties)};
}

BuildResult BuildOne(const TypeDescription& type) {
    return BuildSchema(std::span<const TypeDescription>(&type, 1));
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// The hash of a type made of the given properties, or 0 if it does not build.
uint64_t HashOf(std::vector<PropertyDescription> properties, uint32_t type = 1) {
    const BuildResult result = BuildOne(Type(type, std::move(properties)));
    return result.schema ? result.schema->Hash() : 0;
}

std::vector<PropertyDescription> Mixed() {
    return {Position("position"), Angle("heading"), Integer("health", 0, 100, false),
            Flag("alive"),        Mask("flags", 8),  Choice("team", 5)};
}

} // namespace

TESSERA_TEST(schema_position_matches_the_example_server_hash) {
    tessera::server::example::ExampleGame game(1, false);
    const uint64_t server_hash = game.DescribeWorld().schema->Hash();
    const BuildResult result = BuildOne(Type(1, {Position("position", 0.01, 2048.0)}));
    TESSERA_CHECK(result.schema != nullptr);
    TESSERA_CHECK(result.error.empty());
    if (result.schema) {
        TESSERA_CHECK(result.schema->Hash() == server_hash);
    }
}

TESSERA_TEST(schema_hash_follows_every_parameter) {
    const uint64_t base = HashOf(Mixed());
    TESSERA_CHECK(base != 0);

    auto changed = [&](auto edit) {
        std::vector<PropertyDescription> properties = Mixed();
        edit(properties);
        const uint64_t hash = HashOf(std::move(properties));
        return hash != 0 && hash != base;
    };
    TESSERA_CHECK(changed([](auto& p) { p[0].step = 0.02; }));
    TESSERA_CHECK(changed([](auto& p) { p[0].range = 1024.0; }));
    TESSERA_CHECK(changed([](auto& p) { p[1].bits = 11; }));
    TESSERA_CHECK(changed([](auto& p) { p[2].min_value = 1; }));
    TESSERA_CHECK(changed([](auto& p) { p[2].max_value = 101; }));
    TESSERA_CHECK(changed([](auto& p) { p[2].is_signed = true; }));
    TESSERA_CHECK(changed([](auto& p) { p[4].bits = 9; }));
    TESSERA_CHECK(changed([](auto& p) { p[5].count = 6; }));
    TESSERA_CHECK(changed([](auto& p) { std::swap(p[0], p[1]); }));
    TESSERA_CHECK(changed([](auto& p) {
        p[3].kind = "choice";
        p[3].count = 2;
    }));
    TESSERA_CHECK(HashOf(Mixed(), 2) != base);
}

TESSERA_TEST(schema_hash_does_not_depend_on_property_names) {
    // The server's schema has no names either, so the hash cannot tell them apart. The guide says so.
    std::vector<PropertyDescription> renamed = Mixed();
    for (PropertyDescription& p : renamed) {
        p.name += "_renamed";
    }
    TESSERA_CHECK(HashOf(renamed) == HashOf(Mixed()));
}

TESSERA_TEST(schema_builds_every_kind) {
    const BuildResult result = BuildOne(Type(1, Mixed()));
    TESSERA_CHECK(result.schema != nullptr);
    if (!result.schema) {
        return;
    }
    const std::vector<PropertyInfo>* info = result.schema->Properties(1);
    TESSERA_CHECK(info != nullptr);
    if (info == nullptr) {
        return;
    }
    TESSERA_CHECK(info->size() == 6);
    const ValueKind kinds[] = {ValueKind::Position, ValueKind::Angle, ValueKind::Integer,
                               ValueKind::Flag,     ValueKind::Mask,  ValueKind::Choice};
    const char* names[] = {"position", "heading", "health", "alive", "flags", "team"};
    for (size_t i = 0; i < info->size() && i < 6; ++i) {
        TESSERA_CHECK((*info)[i].kind == kinds[i]);
        TESSERA_CHECK((*info)[i].name == names[i]);
    }
    // A position is 3 words, an angle 1, an integer 2, a flag and a mask 2 each, a choice 1.
    TESSERA_CHECK(result.schema->Schema().Find(1)->words == 3 + 1 + 2 + 2 + 2 + 1);
    TESSERA_CHECK(result.schema->Properties(2) == nullptr);
}

TESSERA_TEST(schema_integer_column_fits_its_range) {
    struct Case {
        int64_t min_value;
        int64_t max_value;
        bool is_signed;
        uint8_t bytes;
    };
    const Case cases[] = {{-128, 127, true, 1},
                          {-129, 127, true, 2},
                          {0, 255, false, 1},
                          {0, 65535, false, 2},
                          {0, 65536, false, 4},
                          {-(int64_t{1} << 40), int64_t{1} << 40, true, 8}};
    for (const Case& c : cases) {
        const BuildResult result = BuildOne(Type(1, {Integer("n", c.min_value, c.max_value, c.is_signed)}));
        TESSERA_CHECK(result.schema != nullptr);
        if (result.schema) {
            TESSERA_CHECK(result.schema->Schema().Find(1)->properties[0].source[0].element_bytes == c.bytes);
        }
    }
}

TESSERA_TEST(schema_unsigned_64_bit_range_builds) {
    const int64_t top = std::numeric_limits<int64_t>::max();
    const BuildResult unsigned_wide = BuildOne(Type(1, {Integer("n", 0, top, false)}));
    TESSERA_CHECK(unsigned_wide.schema != nullptr);
    const BuildResult signed_wide =
        BuildOne(Type(1, {Integer("n", std::numeric_limits<int64_t>::min(), top, true)}));
    TESSERA_CHECK(signed_wide.schema != nullptr);
    // An unsigned number cannot start below zero.
    const BuildResult negative = BuildOne(Type(1, {Integer("n", -1, 10, false)}));
    TESSERA_CHECK(negative.schema == nullptr);
    TESSERA_CHECK(Contains(negative.error, "\"min\""));
}

TESSERA_TEST(schema_rejects_unknown_kind_naming_field) {
    PropertyDescription unknown;
    unknown.name = "hp";
    unknown.kind = "float";
    const BuildResult result = BuildOne(Type(1, {Position("position"), Flag("ok"), unknown}));
    TESSERA_CHECK(result.schema == nullptr);
    TESSERA_CHECK(Contains(result.error, "type 1, property \"hp\": unknown kind \"float\""));
    for (const char* kind : {"position", "angle", "integer", "flag", "mask", "choice"}) {
        TESSERA_CHECK(Contains(result.error, kind));
    }
}

TESSERA_TEST(schema_rejects_empty_range) {
    const BuildResult backwards = BuildOne(Type(1, {Integer("hp", 5, 4, true)}));
    TESSERA_CHECK(backwards.schema == nullptr);
    TESSERA_CHECK(Contains(backwards.error, "\"min\" is above \"max\""));
    TESSERA_CHECK(Contains(backwards.error, "property \"hp\""));

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const PropertyDescription bad[] = {Position("p", 0.01, 0.0),   Position("p", 0.0, 2048.0),
                                       Position("p", nan, 2048.0), Position("p", 0.01, inf),
                                       Position("p", -0.01, 2048.0)};
    for (const PropertyDescription& p : bad) {
        const BuildResult result = BuildOne(Type(1, {p}));
        TESSERA_CHECK(result.schema == nullptr);
        TESSERA_CHECK(Contains(result.error, "type 1, property \"p\":"));
    }
    const BuildResult huge = BuildOne(Type(1, {Position("p", 1e-9, 2048.0)}));
    TESSERA_CHECK(huge.schema == nullptr);
    TESSERA_CHECK(Contains(huge.error, "too large"));
}

TESSERA_TEST(schema_rejects_duplicate_property_name) {
    const BuildResult result = BuildOne(Type(1, {Position("position"), Flag("position")}));
    TESSERA_CHECK(result.schema == nullptr);
    TESSERA_CHECK(Contains(result.error, "type 1, property \"position\":"));
    TESSERA_CHECK(Contains(result.error, "more than once"));
}

TESSERA_TEST(schema_rejects_empty_property_name) {
    const BuildResult result = BuildOne(Type(1, {Flag("")}));
    TESSERA_CHECK(result.schema == nullptr);
    TESSERA_CHECK(Contains(result.error, "type 1, property 0: the name is empty"));
}

TESSERA_TEST(schema_rejects_type_without_properties) {
    const BuildResult result = BuildOne(Type(1, {}));
    TESSERA_CHECK(result.schema == nullptr);
    TESSERA_CHECK(result.error == "type 1: needs at least one property");
    TESSERA_CHECK(ValidateType(Type(1, {})) == "type 1: needs at least one property");
}

TESSERA_TEST(schema_rejects_more_than_64_properties_and_more_than_32_words) {
    std::vector<PropertyDescription> many;
    for (int i = 0; i < 65; ++i) {
        many.push_back(Angle("a" + std::to_string(i)));
    }
    const BuildResult too_many = BuildOne(Type(3, many));
    TESSERA_CHECK(too_many.schema == nullptr);
    TESSERA_CHECK(Contains(too_many.error, "type 3: "));
    TESSERA_CHECK(Contains(too_many.error, "at most 64"));

    std::vector<PropertyDescription> wide;
    for (int i = 0; i < 11; ++i) {
        wide.push_back(Position("p" + std::to_string(i)));
    }
    const BuildResult too_wide = BuildOne(Type(4, wide));
    TESSERA_CHECK(too_wide.schema == nullptr);
    TESSERA_CHECK(Contains(too_wide.error, "type 4: "));
    TESSERA_CHECK(Contains(too_wide.error, "words"));
    // The check of one type alone finds the same.
    TESSERA_CHECK(Contains(ValidateType(Type(4, wide)), "type 4: "));
}

TESSERA_TEST(schema_rejects_type_above_255_and_a_type_described_twice) {
    const BuildResult high = BuildOne(Type(256, {Flag("f")}));
    TESSERA_CHECK(high.schema == nullptr);
    TESSERA_CHECK(Contains(high.error, "type 256"));
    TESSERA_CHECK(BuildOne(Type(255, {Flag("f")})).schema != nullptr);

    const TypeDescription twice[] = {Type(1, {Flag("f")}), Type(1, {Angle("a")})};
    const BuildResult result = BuildSchema(twice);
    TESSERA_CHECK(result.schema == nullptr);
    TESSERA_CHECK(Contains(result.error, "type 1 is described twice"));
}

TESSERA_TEST(schema_bits_and_choice_bounds) {
    TESSERA_CHECK(BuildOne(Type(1, {Mask("m", 0)})).schema == nullptr);
    TESSERA_CHECK(BuildOne(Type(1, {Mask("m", 65)})).schema == nullptr);
    TESSERA_CHECK(BuildOne(Type(1, {Angle("a", 0)})).schema == nullptr);
    TESSERA_CHECK(BuildOne(Type(1, {Angle("a", 17)})).schema == nullptr);
    TESSERA_CHECK(BuildOne(Type(1, {Choice("c", 1)})).schema == nullptr);
    TESSERA_CHECK(BuildOne(Type(1, {Choice("c", 0)})).schema == nullptr);

    TESSERA_CHECK(BuildOne(Type(1, {Mask("m", 1)})).schema != nullptr);
    TESSERA_CHECK(BuildOne(Type(1, {Mask("m", 64)})).schema != nullptr);
    TESSERA_CHECK(BuildOne(Type(1, {Angle("a", 1)})).schema != nullptr);
    TESSERA_CHECK(BuildOne(Type(1, {Angle("a", 16)})).schema != nullptr);
    TESSERA_CHECK(BuildOne(Type(1, {Choice("c", 2)})).schema != nullptr);
    TESSERA_CHECK(BuildOne(Type(1, {Choice("c", 0xFFFFFFFFu)})).schema != nullptr);
}

TESSERA_TEST(schema_error_leaves_no_schema) {
    const BuildResult bad = BuildOne(Type(1, {}));
    TESSERA_CHECK(bad.schema == nullptr);
    TESSERA_CHECK(!bad.error.empty());
    const BuildResult good = BuildOne(Type(1, {Flag("f")}));
    TESSERA_CHECK(good.schema != nullptr);
    TESSERA_CHECK(good.error.empty());
    // No types at all is not a schema either.
    const BuildResult nothing = BuildSchema({});
    TESSERA_CHECK(nothing.schema == nullptr);
    TESSERA_CHECK(!nothing.error.empty());
}

TESSERA_TEST(schema_built_twice_has_the_same_hash_and_independent_storage) {
    const TypeDescription types[] = {Type(2, {Angle("a")}), Type(1, Mixed())};
    const BuildResult first = BuildSchema(types);
    const BuildResult second = BuildSchema(types);
    TESSERA_CHECK(first.schema != nullptr && second.schema != nullptr);
    if (!first.schema || !second.schema) {
        return;
    }
    TESSERA_CHECK(first.schema->Hash() == second.schema->Hash());
    TESSERA_CHECK(&first.schema->Schema() != &second.schema->Schema());
    // The order the types are given in does not change the schema.
    const TypeDescription reversed[] = {Type(1, Mixed()), Type(2, {Angle("a")})};
    const BuildResult third = BuildSchema(reversed);
    TESSERA_CHECK(third.schema != nullptr && third.schema->Hash() == first.schema->Hash());
}

TESSERA_TEST(schema_players_description_matches_the_example_server_hash) {
    tessera::server::example::ExampleGame game(1, false, true);
    const uint64_t server_hash = game.DescribeWorld().schema->Hash();
    const TypeDescription types[] = {
        Type(1, {Position("position", 0.01, 2048.0)}),
        Type(2, {Position("position", 0.01, 2048.0), Integer("owner", 0, 65535, false)})};
    const BuildResult result = BuildSchema(std::span<const TypeDescription>(types, 2));
    TESSERA_CHECK(result.schema != nullptr && result.error.empty());
    if (result.schema) {
        TESSERA_CHECK(result.schema->Hash() == server_hash);
        TESSERA_CHECK(result.schema->Hash() != HashOf({Position("position")}));
    }
}
