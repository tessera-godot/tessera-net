// SPDX-License-Identifier: Apache-2.0
// A table, a state store, one connection and one receiver wired together, for tests of the whole path.
#pragma once

#include "replication_test_support.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/relevancy/relevancy_types.hpp"
#include "tessera/net/replication/connection_state.hpp"
#include "tessera/net/replication/receiver_state.hpp"
#include "tessera/net/replication/state_store.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace replication_test {

struct Rig {
    ReplicationSchema schema = MakeTestSchema();
    StateStore store{schema};
    ThreadPoolJobSystem jobs{1};
    EntityTable<TestColumns> table{TableConfig{0, kMobType}};
    ConnectionState conn;
    ReceiverState receiver;

    explicit Rig(uint32_t max_visible = 1000, uint32_t max_age = 32)
        : conn(schema, max_visible, max_age), receiver(schema, max_visible, max_age) {}

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    struct Ref {
        TestColumns& columns;
        uint32_t slot;
    };

    Ref At(EntityId id) {
        const auto where = table.Find(id);
        return Ref{table.Columns(where->page), where->slot};
    }

    // Creates an entity, writes its position and flags it as changed.
    EntityId Spawn(float x, float y, float z) {
        const EntityId id = table.Create();
        Ref ref = At(id);
        ref.columns.x[ref.slot] = x;
        ref.columns.y[ref.slot] = y;
        ref.columns.z[ref.slot] = z;
        table.MarkDirty(id);
        return id;
    }

    void MoveX(EntityId id, float x) {
        Ref ref = At(id);
        ref.columns.x[ref.slot] = x;
        table.MarkDirty(id);
    }

    void Enter(EntityId id, uint32_t compact_id) {
        const VisibilityEvent event{VisibilityEvent::Kind::Enter, id, compact_id};
        conn.ApplyEvents(std::span<const VisibilityEvent>(&event, 1));
        receiver.OnEnter(compact_id, id);
    }

    void Leave(EntityId id, uint32_t compact_id) {
        const VisibilityEvent event{VisibilityEvent::Kind::Leave, id, compact_id};
        conn.ApplyEvents(std::span<const VisibilityEvent>(&event, 1));
        receiver.OnLeave(compact_id);
    }

    // Quantizes what changed and tells the connection about it.
    void Refresh() {
        store.Refresh(table, jobs);
        conn.MarkChanged(store.Changed());
        store.ClearChanged();
    }

    std::vector<uint8_t> Encode(uint64_t tick, EncodeStats* stats = nullptr) {
        std::vector<uint8_t> out;
        const EncodeStats written = conn.Encode(tick, store, out);
        if (stats != nullptr) {
            *stats = written;
        }
        return out;
    }

    // True when the receiver's newest state of `compact_id` is the store's state of the entity.
    bool ReceiverMatchesStore(EntityId id, uint32_t compact_id) const {
        const uint32_t* mine = receiver.Current(compact_id);
        const uint32_t* theirs = store.Find(id);
        return mine != nullptr && theirs != nullptr &&
               std::memcmp(mine, theirs, schema.Find(id.type())->words * sizeof(uint32_t)) == 0;
    }
};

} // namespace replication_test
