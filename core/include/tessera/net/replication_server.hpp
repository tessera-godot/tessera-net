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

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
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
    /// Most new inputs handed to the game per player in one Update, 1 to 64; the rest are dropped and
    /// counted.
    uint32_t max_inputs_per_tick = 8;
    /// What one player may be sent: an average of `rate_kbps` kilobits per second (0 means no limit) and,
    /// over any 10 seconds, at most `peak_kbps` (at least the rate). A buffer is never larger than
    /// `max_buffer_bytes`, 64 to 65,535; the default fits one packet of the usual 1,200 bytes.
    uint32_t rate_kbps = 100;
    uint32_t peak_kbps = 200;
    uint32_t max_buffer_bytes = 1100;
    /// An entity that was sent and not confirmed is sent again after the player's measured round trip plus
    /// `resend_margin_ticks`, between `min_resend_ticks` and `max_resend_ticks`; `initial_resend_ticks` is
    /// used until a round trip has been measured.
    uint32_t resend_margin_ticks = 1;
    uint32_t min_resend_ticks = 2;
    uint32_t max_resend_ticks = 30;
    uint32_t initial_resend_ticks = 6;
    /// Priority of a type when the budget does not hold everything (1 to 255), multiplied by the ticks an
    /// entity has waited.
    std::array<uint8_t, 256> type_weight = [] {
        std::array<uint8_t, 256> weights{};
        weights.fill(1);
        return weights;
    }();
};

struct ServerStats {
    uint64_t handshakes_accepted = 0;
    uint64_t handshakes_rejected = 0;
    uint64_t protocol_errors = 0;    ///< Peers disconnected for breaking the protocol.
    uint64_t malformed_messages = 0; ///< Messages that could not be read, or came on the wrong channel.
    uint64_t messages_ignored = 0;   ///< Readable messages that make no sense at that point.
    uint64_t state_buffers_sent = 0;
    uint64_t event_batches_sent = 0;
    uint64_t inputs_delivered = 0; ///< Inputs handed to the game.
    uint64_t inputs_repeated = 0;  ///< Inputs that had already been delivered (the repeats a client sends).
    uint64_t inputs_old = 0;     ///< Inputs older than the newest delivered, and never delivered themselves.
    /// Drop events: a new input over the per-tick limit. The same input counts again each time a later message
    /// repeats it and it is dropped again.
    uint64_t inputs_dropped = 0;
    /// State buffers sent with nothing to say about entities, only to confirm inputs.
    uint64_t input_confirmations_sent = 0;
};

/// Called for each new input of a player, in the order the player sent them and never twice. The bytes are
/// only valid during the call.
using InputHandler =
    std::function<void(ConnectionId player, uint32_t sequence, std::span<const uint8_t> bytes)>;

/// The server's side of the network phase of a tick: it reads the transport (greetings, confirmations,
/// disconnections), runs the relevancy engine, sends each player the visibility events and the state buffer,
/// and passes the confirmations on to the replication engine (queued while it reads, applied when it is done). It owns the relevancy and the replication engine; the
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

    /// Called from `Update`, when a player has been accepted and when one has gone (for any reason). They run
    /// while the transport is being read, before the confirmations read in the same `Update` are applied (those
    /// are applied together when the reading ends), so the state of the other players in
    /// `Replication().Connection(other)` does not yet show them.
    void SetCallbacks(std::function<void(ConnectionId)> on_connected,
                      std::function<void(ConnectionId)> on_disconnected);

    /// Where the inputs of the players go, called from `Update` while the transport is being read. Without a
    /// handler the server still judges and counts the inputs.
    void SetInputHandler(InputHandler handler);

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

    /// The network phase of one tick. `tick` must grow. The inputs this call reads are first simulated on the
    /// next tick, so the state it sends reflects the inputs read by the previous calls.
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
        uint64_t batches_sent = 0;    // event batches sent to this player so far
        AckWindow acks;               // the ticks this player has confirmed
        bool sent_state = false;      // a state buffer has gone to this player
        uint64_t last_state_tick = 0; // the tick of the newest one: an ack beyond it is not believed
        InputWindow inputs;           // which inputs of this player were delivered
        uint64_t input_tick = 0;      // the tick `inputs_this_tick` counts for
        bool input_tick_set = false;
        uint32_t inputs_this_tick = 0; // new inputs delivered for this player in `input_tick`
        uint32_t state_input = 0;      // the newest input that the state of the next buffer reflects
        uint32_t last_sent_input = 0;  // the one that the last buffer sent to this player carried
    };

    static RelevancyConfig PrepareRelevancy(RelevancyConfig relevancy, const ServerConfig& config,
                                            const ReplicationSchema& schema);
    static ReplicationConfig MakeReplicationConfig(const ServerConfig& config);

    void ProcessTransport();
    void SendEventBatches(uint64_t tick);
    void SendStateBuffers(uint64_t tick);
    void CommitInputs();
    void HandleMessage(TransportConnection connection, Peer& peer, const TransportEvent& event);
    void HandleHello(TransportConnection connection, Peer& peer, const ClientHello& hello);
    void HandleInput(Peer& peer, const TransportEvent& event);
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
    uint64_t now_tick_ = 0; // the tick `Update` is running, for measuring how long a confirmation took
    std::function<void(ConnectionId)> on_connected_;
    std::function<void(ConnectionId)> on_disconnected_;
    InputHandler on_input_;
    ServerStats stats_;
    std::vector<TransportEvent> events_; // scratch
};

} // namespace tessera::net
