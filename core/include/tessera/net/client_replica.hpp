// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/entity_id.hpp"
#include "tessera/net/protocol.hpp"
#include "tessera/net/replication/property_schema.hpp"
#include "tessera/net/replication/receiver_state.hpp"
#include "tessera/net/transport/transport.hpp"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace tessera::net {

enum class ClientState : uint8_t {
    Connecting,   ///< Waiting for the transport to open the connection.
    Handshaking,  ///< The hello was sent; waiting for the server's.
    Connected,    ///< The server accepted: events and state are being applied.
    Rejected,     ///< The server refused the hello (see `Rejection`).
    Disconnected, ///< The connection ended, or the server broke the protocol.
};

struct ClientStats {
    uint64_t buffers_applied = 0; ///< State buffers applied whole.
    uint64_t buffers_refused =
        0; ///< State buffers the receiver refused (late, stale, or against an unknown state).
    uint64_t buffers_held = 0;    ///< State buffers that had to wait for their event batches.
    uint64_t buffers_dropped = 0; ///< Held buffers thrown away because too many were waiting.
    uint64_t protocol_errors = 0; ///< Times the server broke the protocol (each ends the connection).
    uint64_t acks_sent = 0;
    uint64_t malformed_messages = 0; ///< Messages that could not be read, or came on the wrong channel.
    uint64_t messages_ignored = 0;   ///< Readable messages that make no sense at that point.
};

/// The player's end of replication: it greets the server, applies the visibility events and the state buffers
/// it sends, and confirms what it applied.
///
/// A server breaks the protocol, and the client disconnects, when a reliable message is unreadable, when an
/// event batch is not the next one in the sequence, or when an event names an entity or a compact id that
/// cannot exist. Garbage on the unreliable channel is simply dropped and counted.
///
/// Call `Poll` once per tick; it is the only thing that reads the transport.
class ClientReplica {
public:
    /// The transport and the schema must outlive the client. The schema must be the one the server uses (the
    /// hello carries its hash, and the server refuses a different one).
    ClientReplica(ITransport& transport, const ReplicationSchema& schema);

    /// Reads what the transport has, and acts on it.
    void Poll();

    ClientState State() const noexcept {
        return state_;
    }
    /// Why the server refused (valid in `Rejected`).
    RejectReason Rejection() const noexcept {
        return rejection_;
    }
    /// The id the server gave this client (valid once connected).
    uint32_t ConnectionId() const noexcept {
        return connection_id_;
    }
    /// How many entities this client can see.
    size_t VisibleCount() const noexcept {
        return compact_of_.size();
    }
    const ClientStats& Stats() const noexcept {
        return stats_;
    }

private:
    void HandleMessage(const TransportEvent& event);
    void HandleServerHello(const ServerHello& hello);
    void HandleEventBatch(const EventBatch& batch);
    void Malformed(Channel channel);
    void Fail();
    void ResetVisible();

    ITransport& transport_;
    const ReplicationSchema& schema_;
    ClientState state_ = ClientState::Connecting;
    RejectReason rejection_ = RejectReason::None;
    uint32_t connection_id_ = 0;
    std::unique_ptr<ReceiverState> receiver_;
    uint64_t batches_applied_ = 0;
    std::vector<EntityId> entity_of_;                   // by compact id; Null when the id is free
    std::unordered_map<uint64_t, uint32_t> compact_of_; // entity -> compact id
    ClientStats stats_;
    std::vector<TransportEvent> events_; // scratch
};

} // namespace tessera::net
