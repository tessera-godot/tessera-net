// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/replication_server.hpp"

#include "tessera/net/profile.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace tessera::net {

namespace {

// The limits a client accepts in the hello (see ClientReplica).
constexpr uint32_t kMaxVisibleLimit = 1u << 18;
constexpr uint32_t kMaxBaselineAgeLimit = 255;
constexpr uint32_t kMaxTickRate = 1000;
constexpr uint32_t kMaxConnectionsLimit = 65535;

} // namespace

RelevancyConfig ReplicationServer::PrepareRelevancy(RelevancyConfig relevancy, const ServerConfig& config,
                                                    const ReplicationSchema& schema) {
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
    // A type that players can see has to be replicable: a client treats an entity of a type it has no schema
    // for as a broken server and disconnects.
    for (uint32_t type = 0; type < relevancy.types.size(); ++type) {
        const TypeRelevancy& rule = relevancy.types[type];
        const bool visible = rule.mode != RelevancyMode::Spatial || rule.range > 0.0f;
        if (visible && schema.Find(type) == nullptr) {
            throw std::invalid_argument("RelevancyConfig: entity type " + std::to_string(type) +
                                        " can be seen by players but has no replication schema");
        }
    }
    if (config.rate_kbps > 1'000'000) {
        throw std::invalid_argument("ServerConfig::rate_kbps must be at most 1000000");
    }
    if (config.peak_kbps < config.rate_kbps || config.peak_kbps > 1'000'000) {
        throw std::invalid_argument("ServerConfig::peak_kbps must be from rate_kbps to 1000000");
    }
    if (config.max_buffer_bytes < 64 || config.max_buffer_bytes > 65535) {
        throw std::invalid_argument("ServerConfig::max_buffer_bytes must be 64 to 65535");
    }
    if (config.resend_margin_ticks > 30) {
        throw std::invalid_argument("ServerConfig::resend_margin_ticks must be 0 to 30");
    }
    if (config.min_resend_ticks == 0 || config.min_resend_ticks > 255) {
        throw std::invalid_argument("ServerConfig::min_resend_ticks must be 1 to 255");
    }
    if (config.max_resend_ticks < config.min_resend_ticks || config.max_resend_ticks > 255) {
        throw std::invalid_argument("ServerConfig::max_resend_ticks must be from min_resend_ticks to 255");
    }
    if (config.initial_resend_ticks < config.min_resend_ticks ||
        config.initial_resend_ticks > config.max_resend_ticks) {
        throw std::invalid_argument(
            "ServerConfig::initial_resend_ticks must be from min_resend_ticks to max_resend_ticks");
    }
    for (const uint8_t weight : config.type_weight) {
        if (weight == 0) {
            throw std::invalid_argument("ServerConfig::type_weight must be 1 to 255 for every type");
        }
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
    replication.tick_rate = config.tick_rate;
    replication.rate_kbps = config.rate_kbps;
    replication.peak_kbps = config.peak_kbps;
    replication.max_buffer_bytes = config.max_buffer_bytes;
    replication.resend_margin_ticks = config.resend_margin_ticks;
    replication.min_resend_ticks = config.min_resend_ticks;
    replication.max_resend_ticks = config.max_resend_ticks;
    replication.initial_resend_ticks = config.initial_resend_ticks;
    replication.type_weight = config.type_weight;
    replication.max_unconfirmed_sends = config.max_unconfirmed_sends;
    return replication;
}

ReplicationServer::ReplicationServer(const ServerConfig& config, RelevancyConfig relevancy,
                                     const ReplicationSchema& schema, SpatialGrid& grid,
                                     RelevancyGroups& groups, IJobSystem& jobs, ITransport& transport)
    : config_(config), schema_(schema), transport_(transport),
      relevancy_(PrepareRelevancy(std::move(relevancy), config, schema), grid, groups, jobs),
      replication_(schema, MakeReplicationConfig(config), jobs), slot_transport_(config.max_connections, 0) {}

void ReplicationServer::SetCallbacks(std::function<void(ConnectionId)> on_connected,
                                     std::function<void(ConnectionId)> on_disconnected) {
    on_connected_ = std::move(on_connected);
    on_disconnected_ = std::move(on_disconnected);
}

void ReplicationServer::Update(uint64_t tick) {
    now_tick_ = tick;
    // Whoever connected, left or confirmed something is dealt with first, so that the rest of the phase only
    // sees the players that are really there.
    {
        TESSERA_PROFILE_SCOPE(Transport);
        ProcessTransport();
    }
    {
        TESSERA_PROFILE_SCOPE(Relevancy);
        relevancy_.Update(tick);
    }
    {
        TESSERA_PROFILE_SCOPE(EventBatches);
        SendEventBatches(tick);
    }
    replication_.Update(tick); // timed inside, in its own stages
    {
        TESSERA_PROFILE_SCOPE(SendBuffers);
        SendStateBuffers(tick);
    }
}

// The visibility events go to the player first and in order, numbered, on the reliable channel, and to the
// replication engine. A state buffer that depends on them is sent after.
void ReplicationServer::SendEventBatches(uint64_t tick) {
    for (ConnectionId id = 0; id < slot_transport_.size(); ++id) {
        if (slot_transport_[id] == 0) {
            continue;
        }
        const std::vector<VisibilityEvent>& events = relevancy_.Events(id).events;
        if (events.empty()) {
            continue; // nothing changed in what this player sees: no message
        }
        Peer& peer = peers_.at(slot_transport_[id]);
        ++peer.batches_sent;
        std::vector<uint8_t> bytes;
        Encode(EventBatch{peer.batches_sent, tick, events}, bytes);
        transport_.Send(slot_transport_[id], Channel::ReliableOrdered, bytes);
        replication_.Charge(id, static_cast<uint32_t>(bytes.size())); // sent, so it counts against the budget
        ++stats_.event_batches_sent;
        replication_.ApplyVisibility(id, events);
        relevancy_.ClearEvents(id);
    }
}

void ReplicationServer::SendStateBuffers(uint64_t tick) {
    for (ConnectionId id = 0; id < slot_transport_.size(); ++id) {
        if (slot_transport_[id] == 0) {
            continue;
        }
        const std::span<const uint8_t> output = replication_.Output(id);
        if (output.size() <= 1) {
            continue; // just the end marker: an idle player costs nothing
        }
        Peer& peer = peers_.at(slot_transport_[id]);
        peer.sent_state = true;
        peer.last_state_tick = tick;
        std::vector<uint8_t> bytes;
        EncodeStateBuffer(StateBufferHeader{tick, peer.batches_sent}, output, bytes);
        transport_.Send(slot_transport_[id], Channel::Unreliable, bytes);
        ++stats_.state_buffers_sent;
    }
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
    // The confirmations of every ack read above are applied now, on all connections at once, before anything
    // that depends on them runs. Until here a connection's state does not yet show them, which includes what
    // the connect and disconnect callbacks can see.
    replication_.ApplyQueuedConfirms();
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
    // An ack for a tick the server has not sent cannot be true. Believing it would move the window of
    // confirmed ticks past every real ack and make the engine look through its pending entities for each tick
    // it names.
    if (!peer.sent_state || ack.latest_tick > peer.last_state_tick) {
        ++stats_.messages_ignored;
        return;
    }
    // Each tick the ack confirms for the first time is confirmed to the replication engine, once.
    const std::vector<uint64_t> fresh = peer.acks.Add(ack);
    for (const uint64_t tick : fresh) {
        replication_.QueueConfirm(peer.id, tick); // applied together at the end of ProcessTransport
    }
    // The newest tick of an ack, the first time it is confirmed, tells how long a round trip takes: the ticks
    // since the buffer was written. The older ticks of the mask were confirmed late by design and would only
    // overstate it.
    if (!fresh.empty() && fresh.back() == ack.latest_tick) {
        const uint64_t waited = now_tick_ - ack.latest_tick; // huge if the ticks went backwards; cut below
        replication_.NoteRoundTrip(
            peer.id, static_cast<uint32_t>(std::min<uint64_t>(waited, config_.max_resend_ticks)));
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
