// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/entity_id.hpp"
#include "tessera/net/replication/property_schema.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tessera::net {

enum class DecodeStatus : uint8_t {
    Ok,
    Truncated,       ///< The buffer ended before the terminator, or inside a record.
    BadRecord,       ///< A record held a value outside its declared range or an impossible number.
    UnknownEntity,   ///< A record named a compact id the receiver was not told about.
    MissingBaseline, ///< A partial record is written against a state the receiver does not have.
    StaleRecord,     ///< The buffer is from before the entity under that compact id became visible.
};

/// The receiving side of replicated state, and the reference decoder: it keeps, for each compact id, the
/// states it received over the last `history_ticks` ticks, because a record is written against the state the
/// server last had confirmed, which may be older than the newest one received.
///
/// Records are applied whole or not at all, and an older record that arrives late is kept as history without
/// replacing a newer state.
class ReceiverState {
public:
    /// `history_ticks` must be at least the sender's `max_baseline_age_ticks`.
    /// The schema must outlive this object and must not be moved.
    ReceiverState(const ReplicationSchema& schema, uint32_t max_visible, uint32_t history_ticks);

    /// An entity became visible under this compact id, at the server tick of the `Enter` event. False for an
    /// id out of range or a type without schema.
    ///
    /// Visibility events must be applied in the order the server produced them. A buffer from before `tick`
    /// cannot be for this entity (the number may have belonged to another one), so it is rejected with
    /// `StaleRecord` instead of being decoded against the wrong type.
    bool OnEnter(uint32_t compact_id, EntityId entity, uint64_t tick = 0);
    /// The entity using this compact id is no longer visible or no longer exists.
    void OnLeave(uint32_t compact_id);

    /// Applies a buffer produced for `tick`. Stops at the first bad record, leaving it and later ones
    /// unapplied; `applied` (if given) receives how many records were applied.
    DecodeStatus Apply(std::span<const uint8_t> buffer, uint64_t tick, size_t* applied = nullptr);

    /// The newest received state of an entity (`words` of its type), or nullptr if none was received yet.
    const uint32_t* Current(uint32_t compact_id) const noexcept;
    /// The tick of that state.
    uint64_t CurrentTick(uint32_t compact_id) const noexcept;
    /// How many received states are kept for an entity.
    size_t HistorySize(uint32_t compact_id) const noexcept;

private:
    struct Entry {
        uint64_t tick;
        std::array<uint32_t, ReplicationSchema::kMaxWordsPerEntity> words;
    };
    struct Slot {
        bool in_use = false;
        uint64_t entered_tick = 0;
        const TypeSchema* type = nullptr;
        std::vector<Entry> history; // ascending by tick
    };

    const ReplicationSchema& schema_;
    uint32_t history_ticks_;
    std::vector<Slot> slots_;
};

} // namespace tessera::net
