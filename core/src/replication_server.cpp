// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/replication_server.hpp"

#include <stdexcept>

namespace tessera::net {

namespace {

// The limits a client accepts in the hello (see ClientReplica).
constexpr uint32_t kMaxVisibleLimit = 1u << 18;
constexpr uint32_t kMaxBaselineAgeLimit = 255;
constexpr uint32_t kMaxTickRate = 1000;
constexpr uint32_t kMaxConnectionsLimit = 65535;

} // namespace

RelevancyConfig ReplicationServer::PrepareRelevancy(RelevancyConfig relevancy, const ServerConfig& config) {
    if (config.max_connections == 0 || config.max_connections > kMaxConnectionsLimit) {
        throw std::invalid_argument("ServerConfig::max_connections must be 1 to 65535");
    }
    if (config.max_visible_per_connection == 0 || config.max_visible_per_connection > kMaxVisibleLimit) {
        throw std::invalid_argument("ServerConfig::max_visible_per_connection must be 1 to 262144");
    }
    if (config.tick_rate == 0 || config.tick_rate > kMaxTickRate) {
        throw std::invalid_argument("ServerConfig::tick_rate must be 1 to 1000");
    }
    if (config.max_baseline_age_ticks == 0 || config.max_baseline_age_ticks > kMaxBaselineAgeLimit) {
        throw std::invalid_argument("ServerConfig::max_baseline_age_ticks must be 1 to 255");
    }
    relevancy.max_connections = config.max_connections;
    relevancy.max_visible_per_connection = config.max_visible_per_connection;
    return relevancy;
}

ReplicationConfig ReplicationServer::MakeReplicationConfig(const ServerConfig& config) {
    ReplicationConfig replication;
    replication.max_connections = config.max_connections;
    replication.max_visible_per_connection = config.max_visible_per_connection;
    replication.max_baseline_age_ticks = config.max_baseline_age_ticks;
    replication.max_unconfirmed_sends = config.max_unconfirmed_sends;
    return replication;
}

ReplicationServer::ReplicationServer(const ServerConfig& config, RelevancyConfig relevancy,
                                     const ReplicationSchema& schema, SpatialGrid& grid,
                                     RelevancyGroups& groups, IJobSystem& jobs, ITransport& transport)
    : config_(config), schema_(schema), transport_(transport),
      relevancy_(PrepareRelevancy(std::move(relevancy), config), grid, groups, jobs),
      replication_(schema, MakeReplicationConfig(config), jobs), slot_transport_(config.max_connections, 0) {}

void ReplicationServer::SetCallbacks(std::function<void(ConnectionId)> on_connected,
                                     std::function<void(ConnectionId)> on_disconnected) {
    on_connected_ = std::move(on_connected);
    on_disconnected_ = std::move(on_disconnected);
}

void ReplicationServer::Update(uint64_t) {
    ProcessTransport();
}

void ReplicationServer::ProcessTransport() {
    events_.clear();
    transport_.Poll(events_);
    for (const TransportEvent& event : events_) {
        switch (event.kind) {
        case TransportEvent::Kind::Connected:
            peers_[event.connection] = Peer{}; // known, but not yet greeted: it takes no slot
            break;
        case TransportEvent::Kind::Disconnected:
            RemovePeer(event.connection);
            break;
        case TransportEvent::Kind::Message: {
            const auto found = peers_.find(event.connection);
            if (found == peers_.end()) {
                ++stats_.messages_ignored; // from a connection that is gone
                break;
            }
            HandleMessage(event.connection, found->second, event);
            break;
        }
        }
    }
}

void ReplicationServer::HandleMessage(TransportConnection connection, Peer& peer,
                                      const TransportEvent& event) {
    MessageKind kind;
    if (!PeekKind(event.data, &kind)) {
        if (event.channel == Channel::ReliableOrdered) {
            Fail(connection);
        } else {
            ++stats_.malformed_messages;
        }
        return;
    }
    // Hellos come on the reliable channel, confirmations on the unreliable one; the server never receives the
    // others.
    const bool right_channel =
        (kind == MessageKind::ClientHello && event.channel == Channel::ReliableOrdered) ||
        (kind == MessageKind::Ack && event.channel == Channel::Unreliable);
    if (!right_channel) {
        if (event.channel == Channel::ReliableOrdered) {
            Fail(connection);
        } else {
            ++stats_.malformed_messages;
        }
        return;
    }
    if (kind == MessageKind::ClientHello) {
        ClientHello hello{};
        if (!Decode(event.data, &hello)) {
            Fail(connection);
            return;
        }
        if (peer.handshaken) {
            Fail(connection); // greeting twice
            return;
        }
        HandleHello(connection, peer, hello);
        return;
    }
    // An Ack.
    Ack ack{};
    if (!Decode(event.data, &ack)) {
        ++stats_.malformed_messages;
        return;
    }
    if (!peer.handshaken) {
        ++stats_.messages_ignored;
        return;
    }
}

void ReplicationServer::HandleHello(TransportConnection connection, Peer& peer, const ClientHello& hello) {
    if (hello.version != kProtocolVersion) {
        Reject(connection, RejectReason::VersionMismatch);
        return;
    }
    if (hello.schema_hash != schema_.Hash()) {
        Reject(connection, RejectReason::SchemaMismatch);
        return;
    }
    ConnectionId id = 0;
    while (id < slot_transport_.size() && slot_transport_[id] != 0) {
        ++id;
    }
    if (id == slot_transport_.size()) {
        Reject(connection, RejectReason::ServerFull);
        return;
    }
    relevancy_.AddConnection(id);
    replication_.AddConnection(id);
    slot_transport_[id] = connection;
    peer.handshaken = true;
    peer.id = id;
    ++connection_count_;
    ++stats_.handshakes_accepted;
    std::vector<uint8_t> reply;
    Encode(ServerHello{true, RejectReason::None, kProtocolVersion, id, config_.tick_rate,
                       config_.max_visible_per_connection, config_.max_baseline_age_ticks},
           reply);
    transport_.Send(connection, Channel::ReliableOrdered, reply);
    if (on_connected_) {
        on_connected_(id);
    }
}

void ReplicationServer::Reject(TransportConnection connection, RejectReason reason) {
    ++stats_.handshakes_rejected;
    std::vector<uint8_t> reply;
    Encode(ServerHello{false, reason, 0, 0, 0, 0, 0}, reply);
    transport_.Send(connection, Channel::ReliableOrdered, reply);
    transport_.Disconnect(connection);
    peers_.erase(connection);
}

void ReplicationServer::Fail(TransportConnection connection) {
    ++stats_.protocol_errors;
    transport_.Disconnect(connection);
    RemovePeer(connection);
}

void ReplicationServer::RemovePeer(TransportConnection connection) {
    const auto found = peers_.find(connection);
    if (found == peers_.end()) {
        return;
    }
    const Peer peer = found->second;
    peers_.erase(found);
    if (!peer.handshaken) {
        return;
    }
    relevancy_.RemoveConnection(peer.id);
    replication_.RemoveConnection(peer.id);
    slot_transport_[peer.id] = 0;
    --connection_count_;
    if (on_disconnected_) {
        on_disconnected_(peer.id);
    }
}

} // namespace tessera::net
