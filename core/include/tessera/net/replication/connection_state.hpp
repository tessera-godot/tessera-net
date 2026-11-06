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

/// A change to the set of entities a connection tracks, as reported by `ConnectionState::LastChanges`.
struct SubscriptionChange {
    bool added;   ///< true: the entity is now tracked under `compact_id`; false: it is no longer tracked
    uint64_t raw; ///< `EntityId::raw()`
    uint32_t compact_id;
};

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
    /// `max_unconfirmed_sends` is how many of the most recent different sends of one entity are remembered,
    /// so that the confirmation of any of them can be used even though the entity was sent again in the
    /// meantime. The schema must outlive this object and must not be moved.
    ConnectionState(const ReplicationSchema& schema, uint32_t max_visible, uint32_t max_baseline_age_ticks,
                    uint32_t max_unconfirmed_sends = 8);

    /// Applies the visibility events of one relevancy `Update`. An entity that enters is tracked with no
    /// confirmed state and is pending, so it is sent in full; one that leaves or is destroyed is forgotten.
    /// Entities whose type has no schema, and compact ids at or above `max_visible`, are not tracked.
    void ApplyEvents(std::span<const VisibilityEvent> events);

    /// What the last `ApplyEvents` call started and stopped tracking, in the order it happened (an entity
    /// that was replaced appears as stopped before the new one starts). Empty for a call that changed
    /// nothing.
    std::span<const SubscriptionChange> LastChanges() const noexcept {
        return changes_;
    }

    /// Flags a compact id as needing to be sent, without looking anything up. Ids that are free or out of
    /// range are ignored.
    void MarkPendingAt(uint32_t compact_id) noexcept;

    /// The entities this connection tracks, ascending by id, with their compact ids.
    std::span<const VisibleEntry> Visible() const noexcept {
        return visible_.Entries();
    }

    /// Flags the tracked entities among `changed` as needing to be sent. Other entities are ignored.
    void MarkChanged(std::span<const EntityId> changed);

    /// Clears `out` and writes this tick's records into it, followed by the terminator. A record is written
    /// for every pending entity that has a quantized state in `store`: complete if the client has confirmed
    /// nothing (or only something older than the baseline age), otherwise only what differs from the
    /// confirmed state. An entity that is not changed and has nothing outstanding is skipped and no longer
    /// pending.
    EncodeStats Encode(uint64_t tick, const StateStore& store, std::vector<uint8_t>& out);

    /// The client confirmed it received the buffer written for `tick`: every entity that was sent in that
    /// buffer (and whose send is still remembered) now has the state sent then as its confirmed state, and
    /// the sends before it are forgotten. Unknown, repeated or forgotten ticks have no effect.
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
        uint64_t acked_tick[kPageIds];
        uint8_t sent_count[kPageIds] = {}; // how many of the remembered sends are valid
        uint8_t sent_head[kPageIds] = {};  // where the next send is written
        uint64_t in_use[kBitWords] = {};
        uint64_t pending[kBitWords] = {};      // the client may not have the current state
        uint64_t stale[kBitWords] = {};        // changed since it was last sent
        uint64_t has_baseline[kBitWords] = {}; // the client has confirmed a state
        uint64_t outstanding[kBitWords] = {};  // a state was sent and is not confirmed yet
        std::vector<uint32_t> acked;           // kPageIds * words per entity
        std::vector<uint32_t> sent;            // kPageIds * ring * words: the latest sends, a ring per entity
        std::vector<uint32_t> sent_runs;       // kPageIds * ring: on how many ticks in a row it was sent
        std::vector<uint64_t> sent_ticks;      // kPageIds * ring: the tick of each of them
    };

    Page* PageOf(uint32_t compact_id, bool create);
    const Page* PageOf(uint32_t compact_id) const noexcept;
    bool ValidEnter(const VisibilityEvent& event) noexcept;
    void EnterOne(uint64_t raw, uint32_t compact_id);
    void Release(uint32_t compact_id);

    const ReplicationSchema& schema_;
    uint32_t max_visible_;
    uint32_t max_baseline_age_;
    uint32_t ring_;
    uint32_t words_;
    VisibleSet visible_;
    std::vector<std::unique_ptr<Page>> pages_;
    uint64_t untracked_enters_ = 0;
    std::vector<SubscriptionChange> changes_;
    std::vector<uint64_t> leaving_; // scratch
    std::vector<VisibleEntry> entering_;
};

} // namespace tessera::net
