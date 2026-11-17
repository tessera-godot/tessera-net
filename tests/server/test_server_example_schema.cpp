// SPDX-License-Identifier: Apache-2.0
// The schema of the example game: its fingerprint is pinned, and a copy of how it used to be written by hand is
// kept here to compare the real one with.
#include "example_game.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/replication/property_schema.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

using namespace tessera::net;
using tessera::server::example::ExampleGame;

namespace {

// The columns and the schema of the example, written by hand as the example once did. They stay here so that the
// schema the example gets from its schema file can be compared with them.
struct HandWrittenEntityColumns {
    float x[kPageSlots];
    float y[kPageSlots];
    float z[kPageSlots];
};

struct HandWrittenPlayerColumns {
    float x[kPageSlots];
    float y[kPageSlots];
    float z[kPageSlots];
    uint16_t owner[kPageSlots];
};

ReplicationSchema HandWrittenSchema(bool players) {
    ReplicationSchema schema;
    schema.AddType(1, {PropertyDescriptor::Position(
                          TESSERA_COLUMN(HandWrittenEntityColumns, x), TESSERA_COLUMN(HandWrittenEntityColumns, y),
                          TESSERA_COLUMN(HandWrittenEntityColumns, z), 0.01f, 2048.0f)});
    if (players) {
        schema.AddType(
            2, {PropertyDescriptor::Position(
                    TESSERA_COLUMN(HandWrittenPlayerColumns, x), TESSERA_COLUMN(HandWrittenPlayerColumns, y),
                    TESSERA_COLUMN(HandWrittenPlayerColumns, z), 0.01f, 2048.0f),
                PropertyDescriptor::Integer(TESSERA_COLUMN(HandWrittenPlayerColumns, owner), false, 0, 65535)});
    }
    return schema;
}

bool SameProperty(const PropertyDescriptor& a, const PropertyDescriptor& b) {
    return a.kind == b.kind && a.step == b.step && a.range == b.range && a.bits == b.bits &&
           a.is_signed == b.is_signed && a.min_value == b.min_value && a.max_value == b.max_value &&
           a.enum_count == b.enum_count && a.words == b.words && a.word_offset == b.word_offset;
}

bool SameType(const TypeSchema* a, const TypeSchema* b) {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    if (a->type != b->type || a->words != b->words || a->properties.size() != b->properties.size()) {
        return false;
    }
    for (size_t i = 0; i < a->properties.size(); ++i) {
        if (!SameProperty(a->properties[i], b->properties[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

TESSERA_TEST(example_schema_hash_without_players_is_pinned) {
    ExampleGame game(1, false);
    TESSERA_CHECK(game.DescribeWorld().schema->Hash() == 1615724867110258213ull);
}

TESSERA_TEST(example_schema_hash_with_players_is_pinned) {
    ExampleGame game(1, false, true);
    TESSERA_CHECK(game.DescribeWorld().schema->Hash() == 339718250426401488ull);
}

TESSERA_TEST(example_schema_matches_the_hand_written_one) {
    for (const bool players : {false, true}) {
        ExampleGame game(1, false, players);
        const ReplicationSchema* actual = game.DescribeWorld().schema;
        const ReplicationSchema expected = HandWrittenSchema(players);
        TESSERA_CHECK(actual->Hash() == expected.Hash());
        TESSERA_CHECK(actual->MaxWords() == expected.MaxWords());
        TESSERA_CHECK(SameType(actual->Find(1), expected.Find(1)));
        TESSERA_CHECK(SameType(actual->Find(2), expected.Find(2)));
        TESSERA_CHECK((actual->Find(2) != nullptr) == players);
    }
}
