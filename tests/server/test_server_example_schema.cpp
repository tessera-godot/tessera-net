// SPDX-License-Identifier: Apache-2.0
// The schema of the example game: its fingerprint is pinned, and a copy of how it used to be written by hand
// is kept here to compare the real one with.
#include "example_game.hpp"
#include "example_schema.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/replication/property_schema.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

using namespace tessera::net;
using tessera::server::example::ExampleGame;
namespace generated = tessera::server::example::schema;

namespace {

// The columns and the schema of the example, written by hand as the example once did. They stay here so that
// the schema the example gets from its schema file can be compared with them.
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
    schema.AddType(
        1, {PropertyDescriptor::Position(TESSERA_COLUMN(HandWrittenEntityColumns, x),
                                         TESSERA_COLUMN(HandWrittenEntityColumns, y),
                                         TESSERA_COLUMN(HandWrittenEntityColumns, z), 0.01f, 2048.0f)});
    if (players) {
        schema.AddType(
            2,
            {PropertyDescriptor::Position(TESSERA_COLUMN(HandWrittenPlayerColumns, x),
                                          TESSERA_COLUMN(HandWrittenPlayerColumns, y),
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

// Compares two values in a way that is not a constant expression, so that a check of two constants does not
// draw a warning.
template <class A, class B> bool Same(const A& a, const B& b) {
    return a == b;
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

// What the example now gets from its schema file (server/example/example_schema.json).
TESSERA_TEST(generated_example_schema_hash_is_the_file_hash) {
    TESSERA_CHECK(Same(generated::kSchemaHash, 339718250426401488ull));
    TESSERA_CHECK(Same(generated::kSchemaHash, HandWrittenSchema(true).Hash()));
    TESSERA_CHECK(!Same(generated::kSchemaHash, HandWrittenSchema(false).Hash()));
    // Only the entity type has the hash the example had before it had players.
    ReplicationSchema only_entity;
    TESSERA_CHECK(generated::AddEntityType(only_entity));
    TESSERA_CHECK(only_entity.Hash() == 1615724867110258213ull);
}

TESSERA_TEST(generated_type_numbers_are_the_ones_the_example_uses) {
    TESSERA_CHECK(Same(generated::kEntityType, 1u));
    TESSERA_CHECK(Same(generated::kPlayerType, 2u));
    TESSERA_CHECK(Same(generated::kPlayerType, ExampleGame::kPlayerType));
    TESSERA_CHECK(Same(generated::kPlayerOwnerMin, int64_t{0}));
    TESSERA_CHECK(Same(generated::kPlayerOwnerMax, int64_t{65535}));
    TESSERA_CHECK(Same(generated::kEntityPositionStep, ExampleGame::kPositionStep));
    TESSERA_CHECK(Same(generated::kEntityPositionRange, ExampleGame::kPositionRange));
    TESSERA_CHECK(Same(generated::kPlayerPositionStep, ExampleGame::kPositionStep));
}

TESSERA_TEST(generated_tables_carry_shard_and_type) {
    const auto players = generated::MakePlayerTable(3);
    const EntityId player = players->Create();
    TESSERA_CHECK(!player.IsNull() && player.shard() == 3 && player.type() == 2);
    const auto entities = generated::MakeEntityTable(9);
    const EntityId entity = entities->Create();
    TESSERA_CHECK(!entity.IsNull() && entity.shard() == 9 && entity.type() == 1);
}
