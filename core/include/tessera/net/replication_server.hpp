// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/entity_table.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/protocol.hpp"
#include "tessera/net/relevancy/relevancy_config.hpp"
#include "tessera/net/relevancy/relevancy_engine.hpp"
#include "tessera/net/relevancy/relevancy_groups.hpp"
#include "tessera/net/replication/property_schema.hpp"
#include "tessera/net/replication/replication_engine.hpp"
#include "tessera/net/spatial_grid.hpp"
#include "tessera/net/transport/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace tessera::net {

struct ServerConfig {
    /// Highest number of players at once (1 to 65,535). Used for the relevancy and the replication engines
    /// alike.
    uint32_t max_connections = 128;
    /// Compact ids per player (1 to 262,144, the most a client accepts).
    uint32_t max_visible_per_connection = 65536;
    /// Sent to clients in the hello (1 to 1,000).
    uint32_t tick_rate = 30;
    /// Oldest confirmed state still used as a reference (1 to 255); clients keep this many ticks of history.
    uint32_t max_baseline_age_ticks = 32;
    /// Recent sends remembered per entity and player (1 to 255); see `ReplicationConfig`.
    uint32_t max_unconfirmed_sends = 8;
};

struct ServerStats {
    uint64_t handshakes_accepted = 0;
    uint64_t handshakes_rejected = 0;
    uint64_t protocol_errors = 0;    ///< Peers disconnected for breaking the protocol.
    uint64_t malformed_messages = 0; ///< Messages that could not be read, or came on the wrong channel.
    uint64_t messages_ignored = 0;   ///< Readable messages that make no sense at that point.
    uint64_t state_buffers_sent = 0;
    uint64_t event_batches_sent = 0;
};

/// The server's side of the network phase of a tick: it reads the transport (greetings, confirmations,
/// disconnections), runs the relevancy engine, sends each player the visibility events and the state buffer,
/// and turns the confirmations into `Confirm` calls. It owns the relevancy and the replication engine; the
/// game owns the simulation (the grid, the groups and the tables) and gives the server references to them.
///
/// Each tick, the game moves its entities and updates the grid, calls `Refresh` for each table and then
/// `Update(tick)`. When a player connects (`on_connected`) the game sets their observers with
/// `Relevancy().SetObservers`; the first `Update` after that already shows them what is near.
class ReplicationServer {
public:
    /// The schema, the grid, the groups, the job system and the transport must outlive the server.
    /// `relevancy` gets `max_connections` and `max_visible_per_connection` from `config`, so the engines
    /// cannot disagree. Throws std::invalid_argument for a config a client would refuse or an engine cannot
    /// use.
    ReplicationServer(const ServerConfig& config, RelevancyConfig relevancy, const ReplicationSchema& schema,
                      SpatialGrid& grid, RelevancyGroups& groups, IJobSystem& jobs, ITransport& transport);

    /// Called from `Update`, when a player has been accepted and when one has gone (for any reason).
    void SetCallbacks(std::function<void(ConnectionId)> on_connected,
                      std::function<void(ConnectionId)> on_disconnected);

    RelevancyEngine& Relevancy() noexcept {
        return relevancy_;
    }
    ReplicationEngine& Replication() noexcept {
        return replication_;
    }

    /// Quantizes the flagged entities of one table. Call it for each table before `Update`.
    template <class Columns> size_t Refresh(EntityTable<Columns>& table) {
        return replication_.Refresh(table);
    }

    /// The network phase of one tick. `tick` must grow.
    void Update(uint64_t tick);

    /// Players that have been accepted and have not left.
    size_t ConnectionCount() const noexcept {
        return connection_count_;
    }
    const ServerStats& Stats() const noexcept {
        return stats_;
    }

private:
    struct Peer {
        bool handshaken = false;
        ConnectionId id = 0;
        uint64_t batches_sent = 0; // event batches sent to this player so far
        AckWindow acks;            // the ticks this player has confirmed
    };

    static RelevancyConfig PrepareRelevancy(RelevancyConfig relevancy, const ServerConfig& config);
    static ReplicationConfig MakeReplicationConfig(const ServerConfig& config);

    void ProcessTransport();
    void SendEventBatches(uint64_t tick);
    void SendStateBuffers(uint64_t tick);
    void HandleMessage(TransportConnection connection, Peer& peer, const TransportEvent& event);
    void HandleHello(TransportConnection connection, Peer& peer, const ClientHello& hello);
    void Reject(TransportConnection connection, RejectReason reason);
    void Fail(TransportConnection connection);
    void RemovePeer(TransportConnection connection);

    ServerConfig config_;
    const ReplicationSchema& schema_;
    ITransport& transport_;
    RelevancyEngine relevancy_;
    ReplicationEngine replication_;
    std::unordered_map<TransportConnection, Peer> peers_;
    std::vector<TransportConnection> slot_transport_; // by connection id; 0 while the slot is free
    size_t connection_count_ = 0;
    std::function<void(ConnectionId)> on_connected_;
    std::function<void(ConnectionId)> on_disconnected_;
    ServerStats stats_;
    std::vector<TransportEvent> events_; // scratch
};

} // namespace tessera::net
