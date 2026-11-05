// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <set>
#include <vector>

#include "tessera/net/entity_table.hpp"
#include "test_support.hpp"

using tessera::net::EntityId;
using tessera::net::EntityTable;
using tessera::net::kPageSlots;
using tessera::net::TableConfig;

namespace {

struct TestColumns {
    float x[kPageSlots];
    uint32_t hp[kPageSlots];
};

using TestTable = EntityTable<TestColumns>;

TableConfig SmallConfig(uint32_t max_slots) {
    TableConfig config;
    config.type = 3;
    config.max_slots = max_slots;
    return config;
}

}  // namespace

TESSERA_TEST(create_returns_distinct_valid_ids) {
    TestTable table(TableConfig{});
    std::set<uint64_t> seen;
    for (int i = 0; i < 100; ++i) {
        const EntityId id = table.Create();
        TESSERA_CHECK(!id.IsNull());
        TESSERA_CHECK(table.IsAlive(id));
        seen.insert(id.raw());
    }
    TESSERA_CHECK(seen.size() == 100);
    TESSERA_CHECK(table.AliveCount() == 100);
}

TESSERA_TEST(destroy_makes_id_stale) {
    TestTable table(TableConfig{});
    const EntityId id = table.Create();
    TESSERA_CHECK(table.Destroy(id));
    TESSERA_CHECK(!table.IsAlive(id));
    TESSERA_CHECK(!table.Destroy(id));
    TESSERA_CHECK(table.AliveCount() == 0);
}

TESSERA_TEST(slot_is_reused_with_new_generation) {
    TestTable table(TableConfig{});
    const EntityId first = table.Create();
    TESSERA_CHECK(table.Destroy(first));
    const EntityId second = table.Create();
    TESSERA_CHECK(second.index() == first.index());
    TESSERA_CHECK(second.generation() == first.generation() + 1);
    TESSERA_CHECK(!table.IsAlive(first));
    TESSERA_CHECK(table.IsAlive(second));
}

TESSERA_TEST(pages_grow_on_demand_and_never_move) {
    TestTable table(TableConfig{});
    const EntityId first = table.Create();
    TESSERA_CHECK(table.PageCount() == 1);
    const TestColumns* first_page = &table.Columns(0);
    for (uint32_t i = 1; i < 2 * kPageSlots + 1; ++i) {
        table.Create();
    }
    TESSERA_CHECK(table.PageCount() == 3);
    TESSERA_CHECK(&table.Columns(0) == first_page);
    TESSERA_CHECK(table.IsAlive(first));
}

TESSERA_TEST(foreign_or_null_ids_are_rejected) {
    TestTable table(TableConfig{});
    const EntityId id = table.Create();
    const EntityId other_type = EntityId::Make(id.shard(), id.type() + 1, id.index(), id.generation());
    const EntityId other_shard = EntityId::Make(id.shard() + 1, id.type(), id.index(), id.generation());
    TESSERA_CHECK(!table.Destroy(EntityId::Null()));
    TESSERA_CHECK(!table.Destroy(other_type));
    TESSERA_CHECK(!table.Destroy(other_shard));
    TESSERA_CHECK(table.IsAlive(id));
}

TESSERA_TEST(capacity_exhaustion_returns_null) {
    TestTable table(SmallConfig(10));
    for (int i = 0; i < 10; ++i) {
        TESSERA_CHECK(!table.Create().IsNull());
    }
    TESSERA_CHECK(table.Create().IsNull());
    TESSERA_CHECK(table.AliveCount() == 10);
}

TESSERA_TEST(for_each_alive_skips_destroyed_slots) {
    TestTable table(TableConfig{});
    std::vector<EntityId> ids;
    for (int i = 0; i < 100; ++i) {
        ids.push_back(table.Create());
    }
    for (size_t i = 0; i < ids.size(); i += 2) {
        table.Destroy(ids[i]);
    }
    size_t visited = 0;
    table.ForEachAliveInPage(0, [&](uint32_t slot) {
        TESSERA_CHECK(table.IsAlive(table.IdOf(0, slot)));
        ++visited;
    });
    TESSERA_CHECK(visited == 50);
}
