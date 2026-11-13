// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/entity_table.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/relevancy/relevancy_types.hpp"
#include "tessera/net/replication/bandwidth_bucket.hpp"
#include "tessera/net/replication/connection_state.hpp"
#include "tessera/net/replication/property_schema.hpp"
#include "tessera/net/replication/round_trip.hpp"
#include "tessera/net/replication/state_store.hpp"
#include "tessera/net/replication/subscription_index.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace tessera::net {

struct ReplicationConfig {
    /// Highest number of connections at once (at most 65,535). Connection ids go from 0 to this number
    /// minus 1.
    uint32_t max_connections = 128;
    /// Number of compact ids per connection (as in `RelevancyConfig::max_visible_per_connection`).
    uint32_t max_visible_per_connection = 65536;
    /// A confirmed state older than this many ticks is not used as a baseline, the entity is sent in full
    /// instead. The receiving side must keep at least this many ticks of history.
    uint32_t max_baseline_age_ticks = 32;
    /// How many of the most recent sends of one entity are remembered per connection (1 to 255), so that the
    /// confirmation of a send can still be used after the entity was sent again. It should cover the
    /// round-trip time in ticks.
    uint32_t max_unconfirmed_sends = 8;
    /// Ticks per second. Turns the bandwidth below into bytes per tick (1 to 1,000).
    uint32_t tick_rate = 30;
    /// Average bytes of state per second each connection may be sent, in kilobits (1,000 bits). 0 means no
    /// budget: every entity that needs sending is written, however large the buffer gets (at most 1,000,000).
    uint32_t rate_kbps = 0;
    /// The most a connection may average over any 10 seconds, in kilobits per second. 0 means the same as
    /// `rate_kbps`; otherwise at least `rate_kbps` (at most 1,000,000). The difference is what a quiet
    /// connection can save up for a burst.
    uint32_t peak_kbps = 0;
    /// The most bytes in one buffer (64 to 65,535), used with a budget. It must also hold the largest
    /// complete record a type can produce, which the constructor checks.
    uint32_t max_buffer_bytes = 65535;
    /// An entity that was sent and not confirmed is sent again after the measured round trip plus this many
    /// ticks (0 to 30), kept between the two limits below.
    uint32_t resend_margin_ticks = 1;
    uint32_t min_resend_ticks = 1;     ///< 1 to 255.
    uint32_t max_resend_ticks = 30;    ///< `min_resend_ticks` to 255.
    uint32_t initial_resend_ticks = 1; ///< Used until a round trip has been measured: between the two limits.
    /// When the budget does not hold everything, an entity's priority is the ticks it has waited times the
    /// weight of its type (1 to 255; the type is `EntityId::type()`).
    std::array<uint8_t, 256> type_weight = [] {
        std::array<uint8_t, 256> weights{};
        weights.fill(1);
        return weights;
    }();
};

/// Runs replicated state for all connections: quantizes what changed in the entity tables, tells each
/// connection about it and writes a buffer of records for each one, all in parallel across connections.
///
/// Per tick, in the Network phase: `Refresh` each table, `ApplyVisibility` with the relevancy events of each
/// connection, `Update`, then read `Output` of each connection and send it. When a client confirms a buffer,
/// call `Confirm`.
class ReplicationEngine {
public:
    /// Throws std::invalid_argument for a config with a zero value. The schema and the job system must
    /// outlive the engine; the schema must not be moved.
    ReplicationEngine(const ReplicationSchema& schema, const ReplicationConfig& config, IJobSystem& jobs);

    /// Returns false for an id out of range or already in use.
    bool AddConnection(ConnectionId connection);
    /// Frees everything kept for a connection. Returns false for an id that is not in use.
    bool RemoveConnection(ConnectionId connection);

    /// Quantizes the entities of `table` that were flagged with `MarkDirty`. Returns how many changed.
    template <class Columns> size_t Refresh(EntityTable<Columns>& table) {
        return store_.Refresh(table, jobs_);
    }

    /// Applies the visibility events the relevancy engine reported for a connection. Unknown connections are
    /// ignored.
    void ApplyVisibility(ConnectionId connection, std::span<const VisibilityEvent> events);

    /// Marks what changed for every connection and writes each connection's buffer. Empties the list of
    /// changes afterwards.
    void Update(uint64_t tick);

    /// The buffer written for a connection by the last `Update` (empty for an unknown connection).
    std::span<const uint8_t> Output(ConnectionId connection) const noexcept;

    /// The client of a connection confirmed the buffer written for `tick`.
    void Confirm(ConnectionId connection, uint64_t tick);

    /// Remembers that the client of a connection confirmed the buffer written for `tick`, to be applied by
    /// `ApplyQueuedConfirms`. The server uses it for the acks it reads, which come in many at once: applying
    /// them together lets the connections work at the same time. Unknown connections are ignored, and what is
    /// queued for a connection is lost with it when it is removed. Call `ApplyQueuedConfirms` before the next
    /// `Update`, or the queue only grows. Do not mix this with `Confirm` on one connection: the immediate call
    /// would run ahead of the ticks already queued.
    void QueueConfirm(ConnectionId connection, uint64_t tick);

    /// Applies every queued confirmation, on all connections at once. Within one connection they are applied
    /// in the order they were queued, as `Confirm` would have, and the queues are empty afterwards. With nothing
    /// queued it does no work.
    void ApplyQueuedConfirms();

    const StateStore& Store() const noexcept {
        return store_;
    }
    /// The client of a connection took `ticks` ticks to confirm a buffer: how long to wait before sending an
    /// unconfirmed entity again follows the average. Unknown connections are ignored.
    void NoteRoundTrip(ConnectionId connection, uint32_t ticks);

    /// Takes `bytes` out of a connection's budget, for what was sent outside the buffer of replicated state
    /// (the event batches). The budget may go into debt. Unknown connections are ignored.
    void Charge(ConnectionId connection, uint32_t bytes);

    /// Ticks an unconfirmed entity of a connection waits before it is sent again; 0 for an unknown
    /// connection.
    uint32_t ResendAfterTicks(ConnectionId connection) const noexcept;

    /// Bytes a connection may be sent right now: the largest `int64_t` when there is no budget, 0 for an
    /// unknown connection.
    int64_t BudgetBytes(ConnectionId connection) const noexcept;

    /// How many entities the last `Update` left out of a connection's buffer because they did not fit in its
    /// byte limit; 0 for an unknown connection.
    uint32_t DeferredEntities(ConnectionId connection) const noexcept;

    /// The configuration the engine was built with.
    const ReplicationConfig& Config() const noexcept {
        return config_;
    }

    /// A connection's state, or nullptr for an unknown connection.
    const ConnectionState* Connection(ConnectionId connection) const noexcept;

    /// How many (entity, connection) pairs the engine is tracking, and the bytes its index of them takes.
    size_t SubscriptionCount() const noexcept {
        return subscriptions_.Count();
    }
    size_t SubscriptionBytes() const noexcept {
        return subscriptions_.MemoryBytes();
    }

    /// What the last `Update` wrote, added up over the connections.
    EncodeStats LastStats() const noexcept {
        return last_stats_;
    }

private:
    struct Entry {
        Entry(const ReplicationSchema& schema, const ReplicationConfig& config)
            : state(schema, config.max_visible_per_connection, config.max_baseline_age_ticks,
                    config.max_unconfirmed_sends),
              bucket(config.rate_kbps, config.peak_kbps == 0 ? config.rate_kbps : config.peak_kbps,
                     config.tick_rate),
              rtt(config.resend_margin_ticks, config.min_resend_ticks, config.max_resend_ticks,
                  config.initial_resend_ticks) {}
        ConnectionState state;
        BandwidthBucket bucket;
        RoundTripEstimator rtt;
        std::vector<uint8_t> output;
        EncodeStats stats;
        std::vector<uint64_t> queued_confirms; // ticks confirmed and not yet applied, in arrival order
    };

    ReplicationConfig config_;
    const ReplicationSchema& schema_;
    IJobSystem& jobs_;
    StateStore store_;
    SubscriptionIndex subscriptions_;
    std::vector<std::unique_ptr<Entry>> connections_;
    std::vector<uint32_t> active_;     // scratch: the ids in use, ascending
    std::vector<uint32_t> confirming_; // scratch: the ids with confirmations queued, ascending
    EncodeStats last_stats_;
};

} // namespace tessera::net
