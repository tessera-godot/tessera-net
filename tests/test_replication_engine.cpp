// SPDX-License-Identifier: Apache-2.0
#include "replication_test_support.hpp"
#include "tessera/net/replication/receiver_state.hpp"
#include "tessera/net/replication/replication_engine.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace tessera::net;
using namespace replication_test;

namespace {

using Table = EntityTable<TestColumns>;

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

VisibilityEvent Enter(EntityId id, uint32_t compact_id) {
    return VisibilityEvent{VisibilityEvent::Kind::Enter, id, compact_id};
}

ReplicationConfig SmallConfig() {
    ReplicationConfig config;
    config.max_connections = 4;
    config.max_visible_per_connection = 1000;
    return config;
}

uint64_t Fnv(uint64_t hash, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        hash ^= (value >> (8 * i)) & 0xFF;
        hash *= 0x100000001B3ull;
    }
    return hash;
}

} // namespace

TESSERA_TEST(add_and_remove_connection_validate_the_id) {
    const ReplicationSchema schema = MakeTestSchema();
    ThreadPoolJobSystem jobs(1);
    ReplicationEngine engine(schema, SmallConfig(), jobs);
    TESSERA_CHECK(engine.AddConnection(0));
    TESSERA_CHECK(!engine.AddConnection(0)); // already in use
    TESSERA_CHECK(!engine.AddConnection(4)); // out of range
    TESSERA_CHECK(engine.AddConnection(3));
    TESSERA_CHECK(!engine.RemoveConnection(1)); // never added
    TESSERA_CHECK(!engine.RemoveConnection(9));
    TESSERA_CHECK(engine.RemoveConnection(0));
    TESSERA_CHECK(!engine.RemoveConnection(0));
    TESSERA_CHECK(engine.Connection(0) == nullptr && engine.Connection(3) != nullptr);
}

TESSERA_TEST(end_to_end_for_one_connection) {
    const ReplicationSchema schema = MakeTestSchema();
    ThreadPoolJobSystem jobs(1);
    ReplicationEngine engine(schema, SmallConfig(), jobs);
    ReceiverState receiver(schema, 1000, 32);
    receiver.AdvanceEvents(~0ull); // this test does not model the delivery of events
    Table table{TableConfig{0, kMobType}};
    TESSERA_CHECK(engine.AddConnection(0));
    const EntityId id = Spawn(table, 1.0f, 2.0f, 3.0f);
    engine.Refresh(table);
    const VisibilityEvent enter = Enter(id, 0);
    engine.ApplyVisibility(0, std::span<const VisibilityEvent>(&enter, 1));
    receiver.OnEnter(0, id);
    engine.Update(1);
    TESSERA_CHECK(receiver.Apply(engine.Output(0), 1) == DecodeStatus::Ok);
    const uint32_t* stored = engine.Store().Find(id);
    TESSERA_CHECK(stored != nullptr && std::equal(stored, stored + 9, receiver.Current(0)));
    engine.Confirm(0, 1);
    MoveX(table, id, 25.0f);
    engine.Refresh(table);
    engine.Update(2);
    TESSERA_CHECK(receiver.Apply(engine.Output(0), 2) == DecodeStatus::Ok);
    TESSERA_CHECK(std::equal(stored, stored + 9, receiver.Current(0)) && receiver.CurrentTick(0) == 2);
}

TESSERA_TEST(update_marks_only_the_connections_that_see_the_entity) {
    const ReplicationSchema schema = MakeTestSchema();
    ThreadPoolJobSystem jobs(1);
    ReplicationEngine engine(schema, SmallConfig(), jobs);
    Table table{TableConfig{0, kMobType}};
    engine.AddConnection(0);
    engine.AddConnection(1);
    const EntityId a = Spawn(table, 1.0f, 0.0f, 0.0f);
    const EntityId b = Spawn(table, 2.0f, 0.0f, 0.0f);
    engine.Refresh(table);
    const VisibilityEvent for_zero = Enter(a, 0);
    const VisibilityEvent for_one = Enter(b, 0);
    engine.ApplyVisibility(0, std::span<const VisibilityEvent>(&for_zero, 1));
    engine.ApplyVisibility(1, std::span<const VisibilityEvent>(&for_one, 1));
    engine.Update(1);
    engine.Confirm(0, 1);
    engine.Confirm(1, 1);
    MoveX(table, a, 50.0f);
    engine.Refresh(table);
    engine.Update(2);
    TESSERA_CHECK(engine.Output(0).size() > 1);  // connection 0 sees the entity that moved
    TESSERA_CHECK(engine.Output(1).size() == 1); // connection 1 only gets the terminator
    TESSERA_CHECK(engine.LastStats().records == 1);
}

TESSERA_TEST(output_is_replaced_by_each_update) {
    const ReplicationSchema schema = MakeTestSchema();
    ThreadPoolJobSystem jobs(1);
    ReplicationEngine engine(schema, SmallConfig(), jobs);
    Table table{TableConfig{0, kMobType}};
    engine.AddConnection(0);
    const EntityId id = Spawn(table, 1.0f, 0.0f, 0.0f);
    engine.Refresh(table);
    const VisibilityEvent enter = Enter(id, 0);
    engine.ApplyVisibility(0, std::span<const VisibilityEvent>(&enter, 1));
    engine.Update(1);
    TESSERA_CHECK(engine.Output(0).size() > 1);
    engine.Confirm(0, 1);
    engine.Update(2);
    TESSERA_CHECK(engine.Output(0).size() == 1);
}

TESSERA_TEST(identical_outputs_with_1_and_8_workers) {
    auto run = [](uint32_t workers) {
        const ReplicationSchema schema = MakeTestSchema();
        ThreadPoolJobSystem jobs(workers);
        ReplicationConfig config;
        config.max_connections = 16;
        config.max_visible_per_connection = 1000;
        ReplicationEngine engine(schema, config, jobs);
        Table table{TableConfig{0, kMobType}};
        std::mt19937 random(2024);
        std::uniform_real_distribution<float> position(-2000.0f, 2000.0f);
        std::vector<EntityId> ids;
        for (int i = 0; i < 1000; ++i) {
            ids.push_back(Spawn(table, position(random), position(random), position(random)));
        }
        engine.Refresh(table);
        for (ConnectionId c = 0; c < 16; ++c) {
            engine.AddConnection(c);
            std::vector<VisibilityEvent> events;
            for (uint32_t i = 0; i < 200; ++i) {
                events.push_back(Enter(ids[(c * 37 + i * 5) % ids.size()], i));
            }
            engine.ApplyVisibility(c, events);
        }
        uint64_t hash = 0xCBF29CE484222325ull;
        for (uint64_t tick = 1; tick <= 50; ++tick) {
            for (int i = 0; i < 150; ++i) {
                MoveX(table, ids[random() % ids.size()], position(random));
            }
            engine.Refresh(table);
            engine.Update(tick);
            for (ConnectionId c = 0; c < 16; ++c) {
                for (const uint8_t byte : engine.Output(c)) {
                    hash = Fnv(hash, byte);
                }
                if (tick >= 2 && (c + tick) % 3 != 0) {
                    engine.Confirm(c, tick - 2); // the confirmations lag, and some are missing
                }
            }
        }
        return hash;
    };
    TESSERA_CHECK(run(1) == run(8));
}

TESSERA_TEST(removed_connection_frees_its_state_and_ignores_later_calls) {
    const ReplicationSchema schema = MakeTestSchema();
    ThreadPoolJobSystem jobs(1);
    ReplicationEngine engine(schema, SmallConfig(), jobs);
    Table table{TableConfig{0, kMobType}};
    engine.AddConnection(2);
    const EntityId id = Spawn(table, 1.0f, 0.0f, 0.0f);
    engine.Refresh(table);
    const VisibilityEvent enter = Enter(id, 0);
    engine.ApplyVisibility(2, std::span<const VisibilityEvent>(&enter, 1));
    engine.Update(1);
    TESSERA_CHECK(engine.RemoveConnection(2));
    engine.ApplyVisibility(2, std::span<const VisibilityEvent>(&enter, 1)); // all of this is harmless
    engine.Confirm(2, 1);
    engine.Update(2);
    TESSERA_CHECK(engine.Output(2).empty());
    engine.ApplyVisibility(99, std::span<const VisibilityEvent>(&enter, 1));
    TESSERA_CHECK(engine.Output(99).empty() && engine.Connection(99) == nullptr);
    TESSERA_CHECK(engine.AddConnection(2)); // the id can be used again, with a clean state
    TESSERA_CHECK(engine.Connection(2)->VisibleCount() == 0);
}

TESSERA_TEST(last_stats_count_the_records_of_the_last_update) {
    const ReplicationSchema schema = MakeTestSchema();
    ThreadPoolJobSystem jobs(1);
    ReplicationEngine engine(schema, SmallConfig(), jobs);
    Table table{TableConfig{0, kMobType}};
    engine.AddConnection(0);
    engine.AddConnection(1);
    std::vector<EntityId> ids;
    std::vector<VisibilityEvent> events;
    for (uint32_t i = 0; i < 5; ++i) {
        ids.push_back(Spawn(table, static_cast<float>(i), 0.0f, 0.0f));
        events.push_back(Enter(ids.back(), i));
    }
    engine.Refresh(table);
    engine.ApplyVisibility(0, events);
    engine.ApplyVisibility(1, events);
    engine.Update(1);
    TESSERA_CHECK(engine.LastStats().records == 10 && engine.LastStats().complete_records == 10);
    engine.Confirm(0, 1);
    engine.Update(2);
    TESSERA_CHECK(engine.LastStats().records == 5); // connection 1 has not confirmed, so it is sent again
}

TESSERA_TEST(type_without_schema_is_not_replicated_and_does_not_break_other_types) {
    const ReplicationSchema schema = MakeTestSchema();
    ThreadPoolJobSystem jobs(1);
    ReplicationEngine engine(schema, SmallConfig(), jobs);
    Table mobs{TableConfig{0, kMobType}};
    Table strangers{TableConfig{0, kUnknownType}};
    engine.AddConnection(0);
    const EntityId mob = Spawn(mobs, 1.0f, 0.0f, 0.0f);
    const EntityId stranger = Spawn(strangers, 2.0f, 0.0f, 0.0f);
    engine.Refresh(mobs);
    engine.Refresh(strangers);
    const VisibilityEvent events[] = {Enter(mob, 0), Enter(stranger, 1)};
    engine.ApplyVisibility(0, events);
    engine.Update(1);
    TESSERA_CHECK(engine.LastStats().records == 1);
    TESSERA_CHECK(engine.Connection(0)->VisibleCount() == 1);
    TESSERA_CHECK(engine.Store().UnreplicatedCount() == 1);
}

TESSERA_TEST(engine_rejects_a_config_that_cannot_work) {
    const ReplicationSchema schema = MakeTestSchema();
    ThreadPoolJobSystem jobs(1);
    auto rejects = [&](auto edit) {
        ReplicationConfig config = SmallConfig();
        edit(config);
        try {
            ReplicationEngine engine(schema, config, jobs);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    TESSERA_CHECK(rejects([](ReplicationConfig& c) { c.max_connections = 0; }));
    TESSERA_CHECK(rejects([](ReplicationConfig& c) { c.max_visible_per_connection = 0; }));
    TESSERA_CHECK(rejects([](ReplicationConfig& c) { c.max_baseline_age_ticks = 0; }));
    TESSERA_CHECK(rejects([](ReplicationConfig& c) { c.max_unconfirmed_sends = 0; }));
    TESSERA_CHECK(rejects([](ReplicationConfig& c) { c.max_unconfirmed_sends = 256; }));
    TESSERA_CHECK(rejects([](ReplicationConfig& c) { c.max_connections = 65'536; }));
    TESSERA_CHECK(!rejects([](ReplicationConfig&) {}));
}

TESSERA_TEST(subscriptions_follow_the_visibility_events) {
    const ReplicationSchema schema = MakeTestSchema();
    ThreadPoolJobSystem jobs(1);
    ReplicationEngine engine(schema, SmallConfig(), jobs);
    Table table{TableConfig{0, kMobType}};
    engine.AddConnection(0);
    engine.AddConnection(1);
    const EntityId a = Spawn(table, 1.0f, 0.0f, 0.0f);
    const EntityId b = Spawn(table, 2.0f, 0.0f, 0.0f);
    engine.Refresh(table);
    const VisibilityEvent both[] = {Enter(a, 0), Enter(b, 1)};
    engine.ApplyVisibility(0, both);
    const VisibilityEvent only_a = Enter(a, 0);
    engine.ApplyVisibility(1, std::span<const VisibilityEvent>(&only_a, 1));
    TESSERA_CHECK(engine.SubscriptionCount() == 3);
    engine.Update(1);
    engine.Confirm(0, 1);
    engine.Confirm(1, 1);
    const VisibilityEvent leave{VisibilityEvent::Kind::Leave, a, 0};
    engine.ApplyVisibility(0, std::span<const VisibilityEvent>(&leave, 1));
    TESSERA_CHECK(engine.SubscriptionCount() == 2);
    MoveX(table, a, 30.0f); // only connection 1 still sees it
    engine.Refresh(table);
    engine.Update(2);
    TESSERA_CHECK(engine.Output(0).size() == 1);
    TESSERA_CHECK(engine.Output(1).size() > 1);
    TESSERA_CHECK(engine.LastStats().records == 1);
}

TESSERA_TEST(removing_a_connection_drops_its_subscriptions) {
    const ReplicationSchema schema = MakeTestSchema();
    ThreadPoolJobSystem jobs(1);
    ReplicationEngine engine(schema, SmallConfig(), jobs);
    Table table{TableConfig{0, kMobType}};
    engine.AddConnection(0);
    engine.AddConnection(1);
    std::vector<VisibilityEvent> events;
    for (uint32_t i = 0; i < 20; ++i) {
        events.push_back(Enter(Spawn(table, static_cast<float>(i), 0.0f, 0.0f), i));
    }
    engine.ApplyVisibility(0, events);
    engine.ApplyVisibility(1, events);
    TESSERA_CHECK(engine.SubscriptionCount() == 40);
    TESSERA_CHECK(engine.RemoveConnection(0));
    TESSERA_CHECK(engine.SubscriptionCount() == 20);
    TESSERA_CHECK(engine.RemoveConnection(1));
    TESSERA_CHECK(engine.SubscriptionCount() == 0);
}

TESSERA_TEST(a_change_to_a_slot_reused_by_a_new_entity_does_not_reach_the_old_viewers) {
    const ReplicationSchema schema = MakeTestSchema();
    ThreadPoolJobSystem jobs(1);
    ReplicationEngine engine(schema, SmallConfig(), jobs);
    Table table{TableConfig{0, kMobType}};
    engine.AddConnection(0);
    engine.AddConnection(1);
    const EntityId first = Spawn(table, 1.0f, 0.0f, 0.0f);
    engine.Refresh(table);
    const VisibilityEvent seen = Enter(first, 0);
    engine.ApplyVisibility(0, std::span<const VisibilityEvent>(&seen, 1));
    engine.Update(1);
    engine.Confirm(0, 1);
    // The entity is destroyed and its slot reused, but connection 0 has not been told yet.
    table.Destroy(first);
    const EntityId second = Spawn(table, 50.0f, 0.0f, 0.0f);
    TESSERA_CHECK(second.index() == first.index());
    const VisibilityEvent sees_second = Enter(second, 0);
    engine.ApplyVisibility(1, std::span<const VisibilityEvent>(&sees_second, 1));
    engine.Refresh(table);
    engine.Update(2);
    // Connection 0 still tracks the old entity, which did not change: it gets nothing.
    TESSERA_CHECK(engine.Output(0).size() == 1);
    TESSERA_CHECK(engine.Output(1).size() > 1);
}

namespace {

// True if building an engine with `config` throws std::invalid_argument naming `field`.
bool RefusedNaming(const ReplicationConfig& config, const char* field,
                   const ReplicationSchema& schema = MakeTestSchema()) {
    ThreadPoolJobSystem jobs(1);
    try {
        ReplicationEngine engine(schema, config, jobs);
    } catch (const std::invalid_argument& error) {
        return std::string(error.what()).find(field) != std::string::npos;
    }
    return false;
}

ReplicationConfig WithBudget() {
    ReplicationConfig config = SmallConfig();
    config.rate_kbps = 100;
    config.peak_kbps = 200;
    config.max_buffer_bytes = 1100;
    return config;
}

} // namespace

TESSERA_TEST(replication_config_rejects_each_new_field_out_of_range) {
    ReplicationConfig config = WithBudget();
    TESSERA_CHECK(!RefusedNaming(config, "tick_rate")); // the budget config itself is fine
    config = WithBudget();
    config.tick_rate = 0;
    TESSERA_CHECK(RefusedNaming(config, "tick_rate"));
    config.tick_rate = 1001;
    TESSERA_CHECK(RefusedNaming(config, "tick_rate"));
    config = WithBudget();
    config.rate_kbps = 1'000'001;
    TESSERA_CHECK(RefusedNaming(config, "rate_kbps"));
    config = WithBudget();
    config.peak_kbps = 50; // below the rate
    TESSERA_CHECK(RefusedNaming(config, "peak_kbps"));
    config.peak_kbps = 1'000'001;
    TESSERA_CHECK(RefusedNaming(config, "peak_kbps"));
    config = WithBudget();
    config.max_buffer_bytes = 63;
    TESSERA_CHECK(RefusedNaming(config, "max_buffer_bytes"));
    config.max_buffer_bytes = 65536;
    TESSERA_CHECK(RefusedNaming(config, "max_buffer_bytes"));
    config = WithBudget();
    config.rate_kbps = 0; // no budget: the cap is not used, so it is not checked
    config.peak_kbps = 0;
    config.max_buffer_bytes = 63;
    TESSERA_CHECK(!RefusedNaming(config, "max_buffer_bytes"));
    config = WithBudget();
    config.resend_margin_ticks = 31;
    TESSERA_CHECK(RefusedNaming(config, "resend_margin_ticks"));
    config = WithBudget();
    config.min_resend_ticks = 0;
    TESSERA_CHECK(RefusedNaming(config, "min_resend_ticks"));
    config.min_resend_ticks = 256;
    TESSERA_CHECK(RefusedNaming(config, "min_resend_ticks"));
    config = WithBudget();
    config.min_resend_ticks = 5;
    config.max_resend_ticks = 4; // below the minimum
    config.initial_resend_ticks = 5;
    TESSERA_CHECK(RefusedNaming(config, "max_resend_ticks"));
    config = WithBudget();
    config.max_resend_ticks = 256;
    TESSERA_CHECK(RefusedNaming(config, "max_resend_ticks"));
    config = WithBudget();
    config.min_resend_ticks = 2;
    config.initial_resend_ticks = 1; // below the minimum
    TESSERA_CHECK(RefusedNaming(config, "initial_resend_ticks"));
    config.initial_resend_ticks = 31; // above the maximum
    TESSERA_CHECK(RefusedNaming(config, "initial_resend_ticks"));
    config = WithBudget();
    config.type_weight[7] = 0;
    TESSERA_CHECK(RefusedNaming(config, "type_weight"));
}

TESSERA_TEST(replication_config_defaults_keep_todays_behaviour) {
    const ReplicationConfig config;
    TESSERA_CHECK(config.rate_kbps == 0 && config.peak_kbps == 0); // no budget
    TESSERA_CHECK(config.min_resend_ticks == 1 && config.initial_resend_ticks == 1); // resend every tick
    TESSERA_CHECK(config.tick_rate == 30 && config.resend_margin_ticks == 1 && config.max_resend_ticks == 30);
    TESSERA_CHECK(config.max_buffer_bytes == 65535);
    for (const uint8_t weight : config.type_weight) {
        TESSERA_CHECK(weight == 1);
    }
}

TESSERA_TEST(engine_refuses_a_buffer_cap_smaller_than_the_largest_record) {
    // Seven 64-bit integers are 14 words, so the largest complete record needs 4 * 14 + 16 = 72 bytes.
    struct WideColumns {
        int64_t v[7][kPageSlots];
    };
    ReplicationSchema schema;
    std::vector<PropertyDescriptor> properties;
    for (int i = 0; i < 7; ++i) {
        properties.push_back(PropertyDescriptor::Integer(
            ColumnRef{static_cast<uint32_t>(offsetof(WideColumns, v) + i * sizeof(int64_t) * kPageSlots), 8}, true,
            std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()));
    }
    TESSERA_CHECK(schema.AddType(3, properties));
    TESSERA_CHECK(schema.MaxWords() == 14);
    ReplicationConfig config = WithBudget();
    config.max_buffer_bytes = 71;
    TESSERA_CHECK(RefusedNaming(config, "max_buffer_bytes", schema));
    config.max_buffer_bytes = 72;
    TESSERA_CHECK(!RefusedNaming(config, "max_buffer_bytes", schema));
    config.rate_kbps = 0; // without a budget the cap is not used
    config.peak_kbps = 0;
    config.max_buffer_bytes = 64;
    TESSERA_CHECK(!RefusedNaming(config, "max_buffer_bytes", schema));
}
