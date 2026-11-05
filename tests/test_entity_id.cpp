// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/entity_id.hpp"
#include "test_support.hpp"

using tessera::net::EntityId;

TESSERA_TEST(entity_id_roundtrips_fields) {
    const EntityId id = EntityId::Make(5, 200, 123456, 77);
    TESSERA_CHECK(id.shard() == 5);
    TESSERA_CHECK(id.type() == 200);
    TESSERA_CHECK(id.index() == 123456);
    TESSERA_CHECK(id.generation() == 77);
    TESSERA_CHECK(!id.IsNull());
}

TESSERA_TEST(entity_id_uses_documented_bit_widths) {
    const EntityId id = EntityId::Make(EntityId::kMaxShard, EntityId::kMaxType, EntityId::kMaxIndex, 0xFFFE);
    TESSERA_CHECK(id.shard() == EntityId::kMaxShard);
    TESSERA_CHECK(id.type() == EntityId::kMaxType);
    TESSERA_CHECK(id.index() == EntityId::kMaxIndex);
    TESSERA_CHECK(id.generation() == 0xFFFE);
    static_assert(EntityId::kMaxShard == 0xFFF);
    static_assert(EntityId::kMaxType == 0xFF);
    static_assert(EntityId::kMaxIndex == (1u << 28) - 1);
}

TESSERA_TEST(null_id_is_all_bits_set_and_never_valid) {
    const EntityId null_id = EntityId::Null();
    TESSERA_CHECK(null_id.raw() == ~0ull);
    TESSERA_CHECK(null_id.IsNull());
    TESSERA_CHECK(null_id.generation() == EntityId::kRetiredGeneration);
}

TESSERA_TEST(ids_compare_by_raw_value) {
    const EntityId a = EntityId::Make(0, 1, 2, 3);
    const EntityId b = EntityId::FromRaw(a.raw());
    const EntityId c = EntityId::Make(0, 1, 2, 4);
    TESSERA_CHECK(a == b);
    TESSERA_CHECK(a != c);
    TESSERA_CHECK(a < c);
}
