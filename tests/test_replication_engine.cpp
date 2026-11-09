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

// Many entities change in one tick and every one of them has to be marked on every connection that sees it, even
// when the workers that mark them share the words of bits (the compact ids are scattered, as the relevancy
// engine hands them out in practice).
TESSERA_TEST(marking_from_eight_workers_loses_no_entity) {
    constexpr uint32_t kEntities = 5000;
    constexpr uint32_t kConnections = 2;
    const ReplicationSchema schema = MakeTestSchema();
    ThreadPoolJobSystem jobs(8);
    ReplicationConfig config;
    config.max_connections = kConnections;
    config.max_visible_per_connection = kEntities;
    ReplicationEngine engine(schema, config, jobs);
    Table table{TableConfig{0, kMobType}};
    std::vector<EntityId> ids;
    for (uint32_t i = 0; i < kEntities; ++i) {
        ids.push_back(Spawn(table, 0.0f, 0.0f, 0.0f));
    }
    engine.Refresh(table);
    for (ConnectionId c = 0; c < kConnections; ++c) {
        engine.AddConnection(c);
        std::vector<VisibilityEvent> events;
        for (uint32_t i = 0; i < kEntities; ++i) {
            events.push_back(Enter(ids[i], (i * 37 + c * 11) % kEntities)); // 37 and 5000 share no factor
        }
        engine.ApplyVisibility(c, events);
    }
    for (uint64_t tick = 1; tick <= 20; ++tick) {
        for (uint32_t i = 0; i < kEntities; ++i) {
            MoveX(table, ids[i], static_cast<float>(tick) * 3.0f + static_cast<float>(i) * 0.01f);
        }
        engine.Refresh(table);
        engine.Update(tick);
        TESSERA_CHECK(engine.LastStats().records == kEntities * kConnections);
        for (ConnectionId c = 0; c < kConnections; ++c) {
            engine.Confirm(c, tick); // everything sent is now confirmed, so only a new mark makes it pending
        }
    }
}

// The same world and the same lagging, partial confirmations as `identical_outputs_with_1_and_8_workers`,
// with several ticks confirmed per connection (repeated, older, and one that was never sent). `queued` hands
// them to the engine to be applied together before the next update; otherwise each one is applied at once.
static uint64_t RunWithConfirmations(uint32_t workers, bool queued) {
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
    const auto confirm = [&](ConnectionId c, uint64_t tick) {
        if (queued) {
            engine.QueueConfirm(c, tick);
        } else {
            engine.Confirm(c, tick);
        }
    };
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
            if (tick >= 4 && (c + tick) % 3 != 0) {
                confirm(c, tick - 3);
                confirm(c, tick - 2);
                confirm(c, tick - 2); // repeated
                confirm(c, 99999);    // never sent
            }
            if (tick >= 3 && c % 4 == 0) {
                confirm(c, tick - 1); // an older tick after a newer one is not an error either
                confirm(c, tick - 2);
            }
        }
        if (queued) {
            engine.ApplyQueuedConfirms();
        }
    }
    return hash;
}

TESSERA_TEST(queued_confirms_leave_the_same_state_as_immediate_ones) {
    const uint64_t immediate = RunWithConfirmations(1, false);
    TESSERA_CHECK(RunWithConfirmations(1, true) == immediate);
    TESSERA_CHECK(RunWithConfirmations(8, true) == immediate); // and the workers do not matter
}

TESSERA_TEST(a_queued_confirm_waits_for_the_apply) {
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
    engine.QueueConfirm(0, 1);
    engine.Update(2);
    TESSERA_CHECK(engine.Output(0).size() > 1); // not confirmed yet: sent again
    engine.ApplyQueuedConfirms();
    engine.Update(3);
    TESSERA_CHECK(engine.Output(0).size() == 1); // confirmed now: nothing to send
}

TESSERA_TEST(a_queued_confirm_does_not_survive_the_removal_of_its_connection) {
    // The confirmation is for tick 2, which the new connection also sends at: if it were applied to the new
    // connection the entity would be confirmed and nothing would be sent at tick 3.
    auto run = [](bool with_stale_confirm) {
        const ReplicationSchema schema = MakeTestSchema();
        ThreadPoolJobSystem jobs(1);
        ReplicationEngine engine(schema, SmallConfig(), jobs);
        Table table{TableConfig{0, kMobType}};
        const EntityId id = Spawn(table, 1.0f, 0.0f, 0.0f);
        engine.Refresh(table);
        const VisibilityEvent enter = Enter(id, 0);
        engine.AddConnection(0);
        engine.ApplyVisibility(0, std::span<const VisibilityEvent>(&enter, 1));
        engine.Update(1);
        if (with_stale_confirm) {
            engine.QueueConfirm(0, 2);
        }
        engine.RemoveConnection(0);
        engine.AddConnection(0);
        engine.ApplyVisibility(0, std::span<const VisibilityEvent>(&enter, 1));
        engine.Update(2);
        const std::vector<uint8_t> at_two(engine.Output(0).begin(), engine.Output(0).end());
        engine.ApplyQueuedConfirms();
        engine.Update(3);
        const std::vector<uint8_t> at_three(engine.Output(0).begin(), engine.Output(0).end());
        return std::pair{at_two, at_three};
    };
    const auto clean = run(false);
    const auto with_queue = run(true);
    TESSERA_CHECK(clean.second.size() > 1); // unconfirmed: sent again at tick 3
    TESSERA_CHECK(with_queue == clean);
}

TESSERA_TEST(applying_with_no_queued_confirms_changes_nothing) {
    auto run = [](bool noisy) {
        const ReplicationSchema schema = MakeTestSchema();
        ThreadPoolJobSystem jobs(8);
        ReplicationEngine engine(schema, SmallConfig(), jobs);
        Table table{TableConfig{0, kMobType}};
        const EntityId id = Spawn(table, 1.0f, 0.0f, 0.0f);
        engine.Refresh(table);
        const VisibilityEvent enter = Enter(id, 0);
        engine.AddConnection(0);
        engine.AddConnection(1);
        engine.ApplyVisibility(0, std::span<const VisibilityEvent>(&enter, 1));
        uint64_t hash = 0xCBF29CE484222325ull;
        for (uint64_t tick = 1; tick <= 6; ++tick) {
            if (noisy) {
                engine.ApplyQueuedConfirms();  // nothing queued
                engine.QueueConfirm(3, tick);  // a connection that was never added
                engine.QueueConfirm(99, tick); // an id out of range
                engine.ApplyQueuedConfirms();
            }
            MoveX(table, id, static_cast<float>(tick));
            engine.Refresh(table);
            engine.Update(tick);
            for (const uint8_t byte : engine.Output(0)) {
                hash = Fnv(hash, byte);
            }
            engine.Confirm(0, tick > 2 ? tick - 2 : 0);
        }
        return hash;
    };
    TESSERA_CHECK(run(true) == run(false));
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
    TESSERA_CHECK(config.rate_kbps == 0 && config.peak_kbps == 0);                   // no budget
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
            ColumnRef{static_cast<uint32_t>(offsetof(WideColumns, v) + i * sizeof(int64_t) * kPageSlots), 8},
            true, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()));
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

namespace {

// `connections` connections that each see all of `entities` mobs, which can all be moved at once.
struct Scene {
    const ReplicationSchema schema = MakeTestSchema();
    ThreadPoolJobSystem jobs;
    ReplicationEngine engine;
    Table table{TableConfig{0, kMobType}};
    std::vector<EntityId> ids;
    uint64_t tick = 0;

    Scene(const ReplicationConfig& config, uint32_t connections, uint32_t entities, uint32_t workers = 1)
        : jobs(workers), engine(schema, config, jobs) {
        for (uint32_t i = 0; i < entities; ++i) {
            ids.push_back(Spawn(table, static_cast<float>(i), 1.0f, 2.0f));
        }
        engine.Refresh(table);
        for (ConnectionId c = 0; c < connections; ++c) {
            engine.AddConnection(c);
            std::vector<VisibilityEvent> events;
            for (uint32_t i = 0; i < entities; ++i) {
                events.push_back(Enter(ids[i], i));
            }
            engine.ApplyVisibility(c, events);
        }
    }

    // Moves every entity, then one tick of the engine.
    void Step(bool move = true) {
        ++tick;
        if (move) {
            for (const EntityId id : ids) {
                MoveX(table, id, static_cast<float>(tick % 97) * 0.7f + static_cast<float>(id.index() % 13));
            }
            engine.Refresh(table);
        }
        engine.Update(tick);
    }
};

ReplicationConfig BudgetConfig() {
    ReplicationConfig config = SmallConfig();
    config.rate_kbps = 20; // about 83 bytes a tick
    config.peak_kbps = 0;  // no savings: the same rate over any window
    config.max_buffer_bytes = 200;
    config.min_resend_ticks = 1;
    return config;
}

} // namespace

TESSERA_TEST(engine_without_a_budget_charges_nothing_and_defers_nothing) {
    Scene scene(SmallConfig(), 1, 20);
    TESSERA_CHECK(scene.engine.BudgetBytes(0) == std::numeric_limits<int64_t>::max());
    uint32_t records = 0;
    for (int i = 0; i < 3; ++i) {
        scene.Step(false); // nothing changes and nothing is confirmed: all 20 are sent on every tick
        records += scene.engine.LastStats().records;
        TESSERA_CHECK(scene.engine.LastStats().deferred == 0 && scene.engine.LastStats().waiting == 0);
    }
    TESSERA_CHECK(records == 60);
    TESSERA_CHECK(scene.engine.BudgetBytes(0) == std::numeric_limits<int64_t>::max());
}

TESSERA_TEST(engine_budget_caps_the_bytes_per_tick_and_the_window) {
    Scene scene(BudgetConfig(), 2, 60);
    std::vector<size_t> sizes;
    for (int i = 0; i < 300; ++i) {
        scene.Step();
        for (ConnectionId c = 0; c < 2; ++c) {
            TESSERA_CHECK(scene.engine.Output(c).size() <= 200);
        }
        sizes.push_back(scene.engine.Output(0).size());
    }
    // 20 kbps is 2,500 bytes a second: over every window of 30 ticks, that and the tick the bucket holds.
    for (size_t start = 0; start + 30 <= sizes.size(); ++start) {
        size_t sum = 0;
        for (size_t i = start; i < start + 30; ++i) {
            sum += sizes[i];
        }
        TESSERA_CHECK(sum <= 2500 + 84);
    }
    TESSERA_CHECK(scene.engine.LastStats().deferred > 0); // there was more to send than the budget allowed
}

TESSERA_TEST(engine_idle_connection_keeps_a_full_bucket) {
    ReplicationConfig config = BudgetConfig();
    config.peak_kbps = 60; // 40 kbps of savings: 50,000 bytes
    Scene scene(config, 1, 0);
    const int64_t full = scene.engine.BudgetBytes(0);
    TESSERA_CHECK(full == 50'000);
    for (int i = 0; i < 100; ++i) {
        scene.Step(false);
    }
    TESSERA_CHECK(scene.engine.BudgetBytes(0) == full);
}

TESSERA_TEST(engine_note_round_trip_changes_the_resend_wait) {
    ReplicationConfig config = SmallConfig();
    Scene scene(config, 1, 1);
    TESSERA_CHECK(scene.engine.ResendAfterTicks(0) == 1);
    scene.engine.NoteRoundTrip(0, 5);
    TESSERA_CHECK(scene.engine.ResendAfterTicks(0) == 6); // 5 ticks and the margin of 1
    scene.Step(false);                                    // sent at tick 1 and never confirmed
    TESSERA_CHECK(scene.engine.LastStats().records == 1);
    for (int i = 0; i < 5; ++i) {
        scene.Step(false);
        TESSERA_CHECK(scene.engine.LastStats().records == 0 && scene.engine.LastStats().waiting == 1);
    }
    scene.Step(false); // tick 7: six ticks after the send
    TESSERA_CHECK(scene.engine.LastStats().records == 1);
}

TESSERA_TEST(engine_charge_reduces_the_budget_of_the_next_tick) {
    ReplicationConfig config = BudgetConfig();
    Scene scene(config, 1, 0);
    const int64_t before = scene.engine.BudgetBytes(0);
    TESSERA_CHECK(before > 0);
    scene.engine.Charge(0, 1000); // more than it had: the bucket is in debt
    TESSERA_CHECK(scene.engine.BudgetBytes(0) == 0);
    scene.Step(false);
    TESSERA_CHECK(scene.engine.BudgetBytes(0) == 0); // one tick of rate is not enough to pay it back
    for (int i = 0; i < 20; ++i) {
        scene.Step(false);
    }
    TESSERA_CHECK(scene.engine.BudgetBytes(0) > 0);
}

TESSERA_TEST(engine_unknown_connection_calls_are_ignored) {
    Scene scene(BudgetConfig(), 1, 1);
    scene.engine.NoteRoundTrip(99, 5);
    scene.engine.Charge(99, 5);
    scene.engine.NoteRoundTrip(2, 5); // in range but never added
    TESSERA_CHECK(scene.engine.ResendAfterTicks(99) == 0 && scene.engine.BudgetBytes(99) == 0);
    TESSERA_CHECK(scene.engine.ResendAfterTicks(2) == 0 && scene.engine.BudgetBytes(2) == 0);
}

TESSERA_TEST(engine_reused_slot_gets_a_full_bucket_and_a_fresh_estimator) {
    ReplicationConfig config = BudgetConfig();
    config.peak_kbps = 60;
    Scene scene(config, 1, 0);
    const int64_t full = scene.engine.BudgetBytes(0);
    const uint32_t initial = scene.engine.ResendAfterTicks(0);
    scene.engine.Charge(0, 40'000);
    scene.engine.NoteRoundTrip(0, 9);
    TESSERA_CHECK(scene.engine.BudgetBytes(0) < full && scene.engine.ResendAfterTicks(0) != initial);
    TESSERA_CHECK(scene.engine.RemoveConnection(0));
    TESSERA_CHECK(scene.engine.AddConnection(0));
    TESSERA_CHECK(scene.engine.BudgetBytes(0) == full && scene.engine.ResendAfterTicks(0) == initial);
}

TESSERA_TEST(engine_budget_results_do_not_depend_on_the_number_of_workers) {
    uint64_t hashes[2] = {};
    int index = 0;
    for (const uint32_t workers : {1u, 8u}) {
        ReplicationConfig config = BudgetConfig();
        config.max_connections = 8;
        Scene scene(config, 6, 80, workers);
        uint64_t hash = 0xcbf29ce484222325ull;
        for (int i = 0; i < 40; ++i) {
            scene.Step();
            for (ConnectionId c = 0; c < 6; ++c) {
                for (const uint8_t byte : scene.engine.Output(c)) {
                    hash = Fnv(hash, byte);
                }
                if (i % 3 == 0) {
                    scene.engine.Confirm(c, scene.tick);
                }
            }
        }
        hashes[index++] = hash;
    }
    TESSERA_CHECK(hashes[0] == hashes[1]);
}

TESSERA_TEST(engine_charges_what_it_writes_so_a_burst_cannot_last) {
    ReplicationConfig config = BudgetConfig();
    config.peak_kbps = 24; // 4 kbps of savings: 5,000 bytes to spend on top of the rate
    Scene scene(config, 1, 60);
    size_t total = 0;
    for (int i = 0; i < 600; ++i) { // 20 seconds, with more to send than the budget allows
        scene.Step();
        total += scene.engine.Output(0).size();
    }
    // 20 kbps for 20 seconds is 50,000 bytes, and 5,000 more saved up, and the tick the bucket always holds.
    TESSERA_CHECK(total <= 50'000 + 5'000 + 84);
    TESSERA_CHECK(total >= 40'000); // and it did use the budget
}

TESSERA_TEST(engine_buffer_cap_applies_even_when_the_bucket_is_full) {
    ReplicationConfig config = BudgetConfig();
    config.peak_kbps = 60; // 50,000 bytes saved: the bucket never limits in this test
    config.max_buffer_bytes = 120;
    Scene scene(config, 1, 60);
    size_t most = 0;
    for (int i = 0; i < 30; ++i) {
        scene.Step();
        most = std::max(most, scene.engine.Output(0).size());
        TESSERA_CHECK(scene.engine.Output(0).size() <= 120);
    }
    TESSERA_CHECK(most > 100); // it really was held back by the cap
}

TESSERA_TEST(engine_passes_the_type_weights_to_the_choice) {
    for (const bool weighted : {false, true}) {
        const ReplicationSchema schema = MakeTestSchema();
        ThreadPoolJobSystem jobs(1);
        ReplicationConfig config = BudgetConfig();
        config.rate_kbps = 1000; // plenty: only the cap limits
        config.max_buffer_bytes = 64;
        if (weighted) {
            config.type_weight[kScoreType] = 255;
        }
        ReplicationEngine engine(schema, config, jobs);
        Table mobs{TableConfig{0, kMobType}};
        Table scores{TableConfig{0, kScoreType}};
        std::vector<VisibilityEvent> events;
        for (uint32_t i = 0; i < 10; ++i) { // mobs take compact ids 0 to 9, scores 10 to 19
            events.push_back(Enter(Spawn(mobs, static_cast<float>(i), 1.0f, 2.0f), i));
        }
        for (uint32_t i = 0; i < 10; ++i) {
            const EntityId score = scores.Create();
            const auto where = scores.Find(score);
            scores.Columns(where->page).score[where->slot] = static_cast<int64_t>(i);
            scores.MarkDirty(score);
            events.push_back(Enter(score, 10 + i));
        }
        engine.Refresh(mobs);
        engine.Refresh(scores);
        engine.AddConnection(0);
        engine.ApplyVisibility(0, events);
        engine.Update(1);
        const ConnectionState& state = *engine.Connection(0);
        TESSERA_CHECK(engine.LastStats().records >= 1 && engine.LastStats().deferred >= 1);
        // Everything waited the same time. With equal weights the lowest ids (the mobs) go first, and what is
        // left of the buffer is filled with the smaller records of the scores; with a heavy score weight the
        // scores go first and no mob fits after them.
        TESSERA_CHECK((state.LastRecordBytes(0) > 0) == !weighted);
        TESSERA_CHECK(!weighted || state.LastRecordBytes(10) > 0);
    }
}

TESSERA_TEST(a_rate_that_cannot_carry_the_largest_record_in_one_tick_is_refused) {
    ReplicationConfig config = WithBudget();
    TESSERA_CHECK(!RefusedNaming(config, "rate_kbps"));
    // 4 kbps is about 16 bytes a tick at 30 Hz: less than a complete record and the headers of a buffer.
    // That record could never be sent, even with a bucket that saves up (the bucket drains under load).
    config.rate_kbps = 4;
    config.peak_kbps = 4;
    TESSERA_CHECK(RefusedNaming(config, "rate_kbps"));
    config.peak_kbps = 40;
    TESSERA_CHECK(RefusedNaming(config, "rate_kbps"));
}
