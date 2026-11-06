// SPDX-License-Identifier: Apache-2.0
#include "replication_test_support.hpp"
#include "tessera/net/replication/state_store.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

using namespace tessera::net;
using namespace replication_test;

namespace {

using Table = EntityTable<TestColumns>;

Table MakeTable(uint32_t type) {
    TableConfig config;
    config.type = type;
    return Table(config);
}

// Creates an entity at a position and flags it as changed, as a game does after writing its columns.
EntityId Spawn(Table& table, float x, float y, float z) {
    const EntityId id = table.Create();
    const auto where = table.Find(id);
    TestColumns& columns = table.Columns(where->page);
    columns.x[where->slot] = x;
    columns.y[where->slot] = y;
    columns.z[where->slot] = z;
    table.MarkDirty(id);
    return id;
}

void MoveX(Table& table, EntityId id, float x) {
    const auto where = table.Find(id);
    table.Columns(where->page).x[where->slot] = x;
    table.MarkDirty(id);
}

uint64_t Fnv(uint64_t hash, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        hash ^= (value >> (8 * i)) & 0xFF;
        hash *= 0x100000001B3ull;
    }
    return hash;
}

} // namespace

TESSERA_TEST(refresh_quantizes_dirty_entities_and_reports_them_in_id_order) {
    const ReplicationSchema schema = MakeTestSchema();
    StateStore store(schema);
    ThreadPoolJobSystem jobs(1);
    Table table = MakeTable(kMobType);
    std::vector<EntityId> ids;
    for (int i = 0; i < 10; ++i) {
        ids.push_back(Spawn(table, 10.0f * static_cast<float>(i), 0.0f, 0.0f));
    }
    // Flag them again in a scrambled order: the report must still be ascending.
    for (const int i : {7, 2, 9, 0, 5, 3, 8, 1, 6, 4}) {
        table.MarkDirty(ids[static_cast<size_t>(i)]);
    }
    TESSERA_CHECK(store.Refresh(table, jobs) == 10);
    const std::span<const EntityId> changed = store.Changed();
    TESSERA_CHECK(changed.size() == 10);
    for (size_t i = 0; i < changed.size(); ++i) {
        TESSERA_CHECK(changed[i] == ids[i]); // ids were created in ascending order
    }
    for (size_t i = 0; i < ids.size(); ++i) {
        const uint32_t* words = store.Find(ids[i]);
        TESSERA_CHECK(words != nullptr);
        TESSERA_CHECK(words != nullptr && words[0] == 1'000'000 + 1000 * i); // x = 10 * i metres, 1 cm steps
    }
}

TESSERA_TEST(refresh_consumes_the_dirty_flags) {
    const ReplicationSchema schema = MakeTestSchema();
    StateStore store(schema);
    ThreadPoolJobSystem jobs(1);
    Table table = MakeTable(kMobType);
    for (int i = 0; i < 5; ++i) {
        Spawn(table, 1.0f, 2.0f, 3.0f);
    }
    TESSERA_CHECK(store.Refresh(table, jobs) == 5);
    TESSERA_CHECK(store.Refresh(table, jobs) == 0);
    TESSERA_CHECK(store.Changed().size() == 5); // the list keeps growing until it is cleared
    store.ClearChanged();
    TESSERA_CHECK(store.Changed().empty());
}

TESSERA_TEST(dirty_entity_with_the_same_quantized_state_is_not_reported) {
    const ReplicationSchema schema = MakeTestSchema();
    StateStore store(schema);
    ThreadPoolJobSystem jobs(1);
    Table table = MakeTable(kMobType);
    const EntityId id = Spawn(table, 100.0f, 0.0f, 0.0f);
    TESSERA_CHECK(store.Refresh(table, jobs) == 1);
    store.ClearChanged();
    MoveX(table, id, 100.001f); // a tenth of a step: same quantized position
    TESSERA_CHECK(store.Refresh(table, jobs) == 0);
    TESSERA_CHECK(store.Changed().empty());
    MoveX(table, id, 100.02f); // two steps
    TESSERA_CHECK(store.Refresh(table, jobs) == 1);
}

TESSERA_TEST(find_returns_null_before_the_entity_was_marked_dirty) {
    const ReplicationSchema schema = MakeTestSchema();
    StateStore store(schema);
    ThreadPoolJobSystem jobs(1);
    Table table = MakeTable(kMobType);
    const EntityId id = table.Create(); // never flagged
    TESSERA_CHECK(store.Refresh(table, jobs) == 0);
    TESSERA_CHECK(store.Find(id) == nullptr);
    table.MarkDirty(id);
    TESSERA_CHECK(store.Refresh(table, jobs) == 1);
    TESSERA_CHECK(store.Find(id) != nullptr);
    TESSERA_CHECK(store.Find(EntityId::Null()) == nullptr);
    TESSERA_CHECK(store.Find(MakeEntity(kMobType, 999'999)) == nullptr);
    TESSERA_CHECK(store.Find(MakeEntity(200, 0)) == nullptr); // a type nobody has stored
}

TESSERA_TEST(find_returns_null_for_a_stale_id_and_finds_the_new_occupant_after_refresh) {
    const ReplicationSchema schema = MakeTestSchema();
    StateStore store(schema);
    ThreadPoolJobSystem jobs(1);
    Table table = MakeTable(kMobType);
    const EntityId first = Spawn(table, 1.0f, 0.0f, 0.0f);
    TESSERA_CHECK(store.Refresh(table, jobs) == 1);
    TESSERA_CHECK(table.Destroy(first));
    const EntityId second = Spawn(table, 50.0f, 0.0f, 0.0f);
    TESSERA_CHECK(second.index() == first.index() && second.generation() != first.generation());
    TESSERA_CHECK(store.Refresh(table, jobs) == 1); // a new occupant is a change even in a used slot
    TESSERA_CHECK(store.Find(first) == nullptr);
    const uint32_t* words = store.Find(second);
    TESSERA_CHECK(words != nullptr && words[0] == 1'000'000 + 5000);
}

TESSERA_TEST(changed_accumulates_over_several_tables_until_cleared) {
    const ReplicationSchema schema = MakeTestSchema();
    StateStore store(schema);
    ThreadPoolJobSystem jobs(1);
    Table mobs = MakeTable(kMobType);
    Table scores = MakeTable(kScoreType);
    const EntityId mob = Spawn(mobs, 1.0f, 2.0f, 3.0f);
    const EntityId score = scores.Create();
    scores.Columns(0).score[0] = 42;
    scores.MarkDirty(score);
    TESSERA_CHECK(store.Refresh(mobs, jobs) == 1);
    TESSERA_CHECK(store.Refresh(scores, jobs) == 1);
    const std::span<const EntityId> changed = store.Changed();
    TESSERA_CHECK(changed.size() == 2 && changed[0] == mob && changed[1] == score); // call order
    store.ClearChanged();
    TESSERA_CHECK(store.Changed().empty());
    TESSERA_CHECK(store.Find(mob) != nullptr && store.Find(score) != nullptr); // clearing keeps the states
}

TESSERA_TEST(refresh_gives_the_same_result_with_1_and_8_workers) {
    auto run = [](uint32_t workers) {
        const ReplicationSchema schema = MakeTestSchema();
        StateStore store(schema);
        ThreadPoolJobSystem jobs(workers);
        Table table = MakeTable(kMobType);
        std::mt19937 random(99);
        std::uniform_real_distribution<float> position(-5000.0f, 5000.0f);
        std::vector<EntityId> ids;
        for (int i = 0; i < 10'000; ++i) { // three pages
            ids.push_back(Spawn(table, position(random), position(random), position(random)));
        }
        uint64_t hash = 0xCBF29CE484222325ull;
        hash = Fnv(hash, store.Refresh(table, jobs));
        for (int tick = 0; tick < 5; ++tick) {
            for (int i = 0; i < 2000; ++i) {
                MoveX(table, ids[random() % ids.size()], position(random));
            }
            hash = Fnv(hash, store.Refresh(table, jobs));
        }
        for (const EntityId id : store.Changed()) {
            hash = Fnv(hash, id.raw());
        }
        for (const EntityId id : ids) {
            const uint32_t* words = store.Find(id);
            for (uint32_t w = 0; w < 9 && words != nullptr; ++w) {
                hash = Fnv(hash, words[w]);
            }
        }
        return hash;
    };
    TESSERA_CHECK(run(1) == run(8));
}

TESSERA_TEST(type_without_schema_is_counted_and_not_stored) {
    const ReplicationSchema schema = MakeTestSchema();
    StateStore store(schema);
    ThreadPoolJobSystem jobs(1);
    Table table = MakeTable(kUnknownType);
    const EntityId a = Spawn(table, 1.0f, 2.0f, 3.0f);
    Spawn(table, 4.0f, 5.0f, 6.0f);
    TESSERA_CHECK(store.Refresh(table, jobs) == 0);
    TESSERA_CHECK(store.UnreplicatedCount() == 2);
    TESSERA_CHECK(store.Find(a) == nullptr);
    TESSERA_CHECK(store.Changed().empty());
    TESSERA_CHECK(store.Refresh(table, jobs) == 0);
    TESSERA_CHECK(store.UnreplicatedCount() == 2); // the dirty flags were consumed
}

TESSERA_TEST(memory_grows_by_whole_pages) {
    const ReplicationSchema schema = MakeTestSchema();
    StateStore store(schema);
    ThreadPoolJobSystem jobs(1);
    Table table = MakeTable(kMobType);
    TESSERA_CHECK(store.MemoryBytes() == 0);
    Spawn(table, 0.0f, 0.0f, 0.0f);
    store.Refresh(table, jobs);
    store.ClearChanged();
    const size_t one_page = store.MemoryBytes();
    TESSERA_CHECK(one_page > 0);
    for (uint32_t i = 1; i < kPageSlots; ++i) {
        Spawn(table, 0.0f, 0.0f, 0.0f);
    }
    store.Refresh(table, jobs);
    store.ClearChanged();
    // The page was allocated whole, so filling it adds only room for the lists of changes, which can have
    // grown to twice what they hold (an id and a slot number for each of the 4,096 entities).
    TESSERA_CHECK(store.MemoryBytes() <= one_page + 2 * kPageSlots * (sizeof(EntityId) + sizeof(uint32_t)));
    Spawn(table, 0.0f, 0.0f, 0.0f); // the 4,097th entity needs a second page
    store.Refresh(table, jobs);
    store.ClearChanged();
    TESSERA_CHECK(store.MemoryBytes() > one_page + kPageSlots * 9 * sizeof(uint32_t));
    // Per entity: 9 words of state and a generation, plus a little for the change list.
    TESSERA_CHECK(one_page / kPageSlots <= 9 * sizeof(uint32_t) + sizeof(uint32_t) + 16);
}
