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

TESSERA_TEST(slot_is_retired_at_generation_limit) {
    TestTable table(SmallConfig(2));
    EntityId id = table.Create();
    const uint32_t retired_index = id.index();
    // Generations 0 to 0xFFFE are valid: destroying the occupant of generation 0xFFFE retires the slot.
    for (uint32_t i = 0; i < EntityId::kRetiredGeneration; ++i) {
        TESSERA_CHECK(table.Destroy(id));
        id = table.Create();
        if (table.RetiredSlotCount() > 0) {
            break;
        }
    }
    TESSERA_CHECK(table.RetiredSlotCount() == 1);
    TESSERA_CHECK(id.index() != retired_index);  // the next creation used the other slot
    const EntityId another = table.Create();
    TESSERA_CHECK(another.IsNull());  // only one live slot is left in a table limited to 2 slots
}

TESSERA_TEST(retired_slot_does_not_count_as_alive) {
    TestTable table(SmallConfig(2));
    EntityId id = table.Create();
    for (uint32_t i = 0; i < EntityId::kRetiredGeneration && table.RetiredSlotCount() == 0; ++i) {
        table.Destroy(id);
        id = table.Create();
    }
    TESSERA_CHECK(table.RetiredSlotCount() == 1);
    TESSERA_CHECK(table.AliveCount() == 1);
}

TESSERA_TEST(mark_dirty_then_consume_visits_each_once) {
    TestTable table(TableConfig{});
    const EntityId a = table.Create();
    const EntityId b = table.Create();
    const EntityId c = table.Create();
    table.MarkDirty(a);
    table.MarkDirty(c);
    table.MarkDirty(c);  // marking twice is the same as once
    (void)b;
    std::vector<uint32_t> visited;
    const size_t count = table.ConsumeDirtyInPage(0, [&](uint32_t slot) { visited.push_back(slot); });
    TESSERA_CHECK(count == 2);
    TESSERA_CHECK(visited.size() == 2);
    TESSERA_CHECK(table.ConsumeDirtyInPage(0, [](uint32_t) {}) == 0);
}

TESSERA_TEST(mark_dirty_on_stale_id_is_ignored) {
    TestTable table(TableConfig{});
    const EntityId id = table.Create();
    table.Destroy(id);
    table.MarkDirty(id);
    TESSERA_CHECK(table.ConsumeDirtyInPage(0, [](uint32_t) {}) == 0);
}

TESSERA_TEST(destroy_clears_dirty_bit) {
    TestTable table(TableConfig{});
    const EntityId id = table.Create();
    table.MarkDirty(id);
    table.Destroy(id);
    const EntityId reused = table.Create();
    TESSERA_CHECK(reused.index() == id.index());
    TESSERA_CHECK(table.ConsumeDirtyInPage(0, [](uint32_t) {}) == 0);
}
