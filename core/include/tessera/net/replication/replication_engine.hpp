// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/entity_table.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/relevancy/relevancy_types.hpp"
#include "tessera/net/replication/connection_state.hpp"
#include "tessera/net/replication/property_schema.hpp"
#include "tessera/net/replication/state_store.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace tessera::net {

struct ReplicationConfig {
    /// Highest number of connections at once. Connection ids go from 0 to this number minus 1.
    uint32_t max_connections = 128;
    /// Number of compact ids per connection (as in `RelevancyConfig::max_visible_per_connection`).
    uint32_t max_visible_per_connection = 65536;
    /// A confirmed state older than this many ticks is not used as a baseline, the entity is sent in full
    /// instead. The receiving side must keep at least this many ticks of history.
    uint32_t max_baseline_age_ticks = 32;
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

    const StateStore& Store() const noexcept {
        return store_;
    }
    /// A connection's state, or nullptr for an unknown connection.
    const ConnectionState* Connection(ConnectionId connection) const noexcept;

    /// What the last `Update` wrote, added up over the connections.
    EncodeStats LastStats() const noexcept {
        return last_stats_;
    }

private:
    struct Entry {
        Entry(const ReplicationSchema& schema, uint32_t max_visible, uint32_t max_age)
            : state(schema, max_visible, max_age) {}
        ConnectionState state;
        std::vector<uint8_t> output;
        EncodeStats stats;
    };

    ReplicationConfig config_;
    const ReplicationSchema& schema_;
    IJobSystem& jobs_;
    StateStore store_;
    std::vector<std::unique_ptr<Entry>> connections_;
    std::vector<uint32_t> active_; // scratch: the ids in use, ascending
    EncodeStats last_stats_;
};

} // namespace tessera::net
