// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/client_replica.hpp"

namespace tessera::net {

namespace {

// What a client is willing to allocate for a server's say-so. A server asking for more is broken or hostile.
constexpr uint32_t kMaxVisibleLimit = 1u << 18;
constexpr uint32_t kMaxBaselineAgeLimit = 255;
constexpr uint32_t kMaxTickRate = 1000;

} // namespace

ClientReplica::ClientReplica(ITransport& transport, const ReplicationSchema& schema)
    : transport_(transport), schema_(schema) {}

void ClientReplica::Poll() {
    events_.clear();
    transport_.Poll(events_);
    for (const TransportEvent& event : events_) {
        if (state_ == ClientState::Disconnected) {
            break; // the connection is over; what is left of this batch no longer matters
        }
        switch (event.kind) {
        case TransportEvent::Kind::Connected:
            if (state_ == ClientState::Connecting) {
                std::vector<uint8_t> hello;
                Encode(ClientHello{kProtocolVersion, schema_.Hash()}, hello);
                transport_.Send(0, Channel::ReliableOrdered, hello);
                state_ = ClientState::Handshaking;
            }
            break;
        case TransportEvent::Kind::Disconnected:
            if (state_ != ClientState::Rejected) {
                state_ = ClientState::Disconnected;
            }
            ResetVisible();
            break;
        case TransportEvent::Kind::Message:
            HandleMessage(event);
            break;
        }
    }
}

void ClientReplica::Malformed(Channel channel) {
    if (channel == Channel::ReliableOrdered) {
        Fail(); // the reliable stream is the server's own doing: if it is unreadable, something is wrong
    } else {
        ++stats_.malformed_messages;
    }
}

void ClientReplica::Fail() {
    ++stats_.protocol_errors;
    transport_.Disconnect(0);
    state_ = ClientState::Disconnected;
    ResetVisible();
}

void ClientReplica::ResetVisible() {
    receiver_.reset();
    entity_of_.clear();
    compact_of_.clear();
}

void ClientReplica::HandleMessage(const TransportEvent& event) {
    MessageKind kind;
    if (!PeekKind(event.data, &kind)) {
        Malformed(event.channel);
        return;
    }
    // Hellos and event batches belong on the reliable channel, state buffers on the unreliable one; the
    // client only ever sends the other two kinds.
    const bool reliable_kind = kind == MessageKind::ServerHello || kind == MessageKind::EventBatch;
    const bool unreliable_kind = kind == MessageKind::StateBuffer;
    if ((reliable_kind && event.channel != Channel::ReliableOrdered) ||
        (unreliable_kind && event.channel != Channel::Unreliable) || (!reliable_kind && !unreliable_kind)) {
        Malformed(event.channel);
        return;
    }
    switch (kind) {
    case MessageKind::ServerHello: {
        if (state_ != ClientState::Handshaking) {
            ++stats_.messages_ignored;
            return;
        }
        ServerHello hello{};
        if (!Decode(event.data, &hello)) {
            Malformed(event.channel);
            return;
        }
        HandleServerHello(hello);
        return;
    }
    case MessageKind::EventBatch: {
        if (state_ != ClientState::Connected) {
            ++stats_.messages_ignored;
            return;
        }
        EventBatch batch;
        if (!Decode(event.data, &batch)) {
            Malformed(event.channel);
            return;
        }
        HandleEventBatch(batch);
        return;
    }
    case MessageKind::StateBuffer:
        ++stats_.messages_ignored;
        return;
    default:
        Malformed(event.channel);
        return;
    }
}

void ClientReplica::HandleServerHello(const ServerHello& hello) {
    if (!hello.accepted) {
        rejection_ = hello.reason;
        state_ = ClientState::Rejected;
        transport_.Disconnect(0);
        return;
    }
    if (hello.version != kProtocolVersion || hello.max_visible == 0 || hello.max_visible > kMaxVisibleLimit ||
        hello.max_baseline_age_ticks == 0 || hello.max_baseline_age_ticks > kMaxBaselineAgeLimit ||
        hello.tick_rate == 0 || hello.tick_rate > kMaxTickRate) {
        Fail();
        return;
    }
    connection_id_ = hello.connection_id;
    receiver_ = std::make_unique<ReceiverState>(schema_, hello.max_visible, hello.max_baseline_age_ticks);
    entity_of_.assign(hello.max_visible, EntityId::Null());
    compact_of_.clear();
    batches_applied_ = 0;
    state_ = ClientState::Connected;
}

void ClientReplica::HandleEventBatch(const EventBatch& batch) {
    if (batch.number != batches_applied_ + 1) {
        Fail(); // batches come on a reliable, ordered channel: a gap or a repeat is the server's fault
        return;
    }
    for (const VisibilityEvent& event : batch.events) {
        const uint32_t compact_id = event.compact_id;
        if (compact_id >= entity_of_.size()) {
            Fail();
            return;
        }
        if (event.kind == VisibilityEvent::Kind::Enter) {
            if (!receiver_->OnEnter(compact_id, event.id, batch.tick)) {
                Fail(); // no schema for that type
                return;
            }
            // The entity may have been visible under another id, and the id may have belonged to another
            // entity.
            if (const auto known = compact_of_.find(event.id.raw()); known != compact_of_.end()) {
                receiver_->OnLeave(known->second);
                entity_of_[known->second] = EntityId::Null();
                compact_of_.erase(known);
            }
            if (!entity_of_[compact_id].IsNull()) {
                compact_of_.erase(entity_of_[compact_id].raw());
            }
            entity_of_[compact_id] = event.id;
            compact_of_[event.id.raw()] = compact_id;
        } else {
            receiver_->OnLeave(compact_id);
            if (!entity_of_[compact_id].IsNull()) {
                compact_of_.erase(entity_of_[compact_id].raw());
                entity_of_[compact_id] = EntityId::Null();
            }
        }
    }
    receiver_->AdvanceEvents(batch.tick);
    ++batches_applied_;
}

} // namespace tessera::net
