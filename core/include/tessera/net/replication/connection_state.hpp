// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/entity_id.hpp"
#include "tessera/net/relevancy/relevancy_types.hpp"
#include "tessera/net/relevancy/visible_set.hpp"
#include "tessera/net/replication/property_schema.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace tessera::net {

class StateStore;

/// What one `ConnectionState::Encode` call wrote.
struct EncodeStats {
    uint32_t records = 0;          ///< Records written (complete and partial).
    uint32_t complete_records = 0; ///< Of those, records that carry every property.
    uint32_t skipped_unquantized = 0;
};

/// What the server knows about one connection's view of the replicated state: which entities it sees (from
/// the relevancy events), which ones are waiting to be sent, and for each the state the client has confirmed
/// and the state sent since. Everything is indexed by the connection's compact id, so the memory used grows
/// with the entities that connection sees, not with the world.
///
/// One connection's state is touched by one thread at a time; different connections can run in parallel.
class ConnectionState {
public:
    /// `max_visible` is the number of compact ids (as in `RelevancyConfig::max_visible_per_connection`).
    /// `max_baseline_age_ticks` is how old a confirmed state may be before the entity is sent in full.
    /// The schema must outlive this object and must not be moved.
    ConnectionState(const ReplicationSchema& schema, uint32_t max_visible, uint32_t max_baseline_age_ticks);

    /// Applies the visibility events of one relevancy `Update`. An entity that enters is tracked with no
    /// confirmed state and is pending, so it is sent in full; one that leaves or is destroyed is forgotten.
    /// Entities whose type has no schema, and compact ids at or above `max_visible`, are not tracked.
    void ApplyEvents(std::span<const VisibilityEvent> events);

    /// Flags the tracked entities among `changed` as needing to be sent. Other entities are ignored.
    void MarkChanged(std::span<const EntityId> changed);

    /// Clears `out` and writes this tick's records into it, followed by the terminator. A record is written
    /// for every pending entity that has a quantized state in `store`: complete if the client has confirmed
    /// nothing (or only something older than the baseline age), otherwise only what differs from the
    /// confirmed state. An entity that is not changed and has nothing outstanding is skipped and no longer
    /// pending.
    EncodeStats Encode(uint64_t tick, const StateStore& store, std::vector<uint8_t>& out);

    /// The client confirmed it received the buffer written for `tick`: every entity whose latest send was in
    /// that buffer now has the sent state as its confirmed state. Unknown, repeated or superseded ticks have
    /// no effect.
    void Confirm(uint64_t tick);

    /// The compact id of a tracked entity, or `kInvalidCompactId`.
    uint32_t CompactIdOf(EntityId id) const noexcept;
    /// The entity using a compact id, or `EntityId::Null()` if the id is free or out of range.
    EntityId EntityOf(uint32_t compact_id) const noexcept;
    bool IsPending(uint32_t compact_id) const noexcept;
    bool HasBaseline(uint32_t compact_id) const noexcept;

    size_t VisibleCount() const noexcept {
        return visible_.Size();
    }
    /// How many `Enter` events were ignored (no schema, or an id out of range).
    uint64_t UntrackedEnters() const noexcept {
        return untracked_enters_;
    }
    size_t MemoryBytes() const noexcept;

private:
    static constexpr uint32_t kPageIds = 256;
    static constexpr uint32_t kBitWords = kPageIds / 64;

    // The records of 256 compact ids, allocated when the first id of the range is used.
    struct Page {
        uint64_t entity[kPageIds];
        uint64_t sent_tick[kPageIds];
        uint64_t acked_tick[kPageIds];
        uint64_t in_use[kBitWords] = {};
        uint64_t pending[kBitWords] = {};      // the client may not have the current state
        uint64_t stale[kBitWords] = {};        // changed since it was last sent
        uint64_t has_baseline[kBitWords] = {}; // the client has confirmed a state
        uint64_t outstanding[kBitWords] = {};  // a state was sent and is not confirmed yet
        std::vector<uint32_t> acked;           // kPageIds * words per entity
        std::vector<uint32_t> sent;
    };

    Page* PageOf(uint32_t compact_id, bool create);
    const Page* PageOf(uint32_t compact_id) const noexcept;
    bool ValidEnter(const VisibilityEvent& event) noexcept;
    void EnterOne(uint64_t raw, uint32_t compact_id);
    void Release(uint32_t compact_id);

    const ReplicationSchema& schema_;
    uint32_t max_visible_;
    uint32_t max_baseline_age_;
    uint32_t words_;
    VisibleSet visible_;
    std::vector<std::unique_ptr<Page>> pages_;
    uint64_t untracked_enters_ = 0;
    std::vector<uint64_t> leaving_; // scratch
    std::vector<VisibleEntry> entering_;
};

} // namespace tessera::net
