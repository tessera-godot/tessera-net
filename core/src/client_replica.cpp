// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/client_replica.hpp"

#include <algorithm>

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
    if (apply_happened_ && state_ == ClientState::Connected) {
        SendAck(); // one confirmation for everything applied in this call
    }
    apply_happened_ = false;
}

void ClientReplica::Disconnect() {
    if (state_ == ClientState::Disconnected) {
        return;
    }
    transport_.Disconnect(0);
    state_ = ClientState::Disconnected;
    ResetVisible();
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
    held_.clear();
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
        if (state_ != ClientState::Connected) {
            ++stats_.messages_ignored;
            return;
        }
        HandleStateBuffer(event);
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
    held_.clear();
    any_applied_ = false;
    applied_latest_ = 0;
    applied_bits_ = 0;
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
    ReleaseHeld();
}

const uint32_t* ClientReplica::WordsOf(EntityId id) const noexcept {
    if (!receiver_) {
        return nullptr;
    }
    const auto known = compact_of_.find(id.raw());
    return known == compact_of_.end() ? nullptr : receiver_->Current(known->second);
}

bool ClientReplica::WasApplied(uint64_t tick) const noexcept {
    if (!any_applied_ || tick > applied_latest_ || applied_latest_ - tick >= 64) {
        return false;
    }
    return ((applied_bits_ >> (applied_latest_ - tick)) & 1ull) != 0;
}

void ClientReplica::MarkApplied(uint64_t tick) noexcept {
    if (!any_applied_ || tick > applied_latest_) {
        const uint64_t shift = any_applied_ ? tick - applied_latest_ : 0;
        applied_bits_ = shift >= 64 ? 0 : applied_bits_ << shift;
        applied_latest_ = tick;
        any_applied_ = true;
    }
    const uint64_t offset = applied_latest_ - tick;
    if (offset < 64) {
        applied_bits_ |= 1ull << offset;
    }
}

void ClientReplica::SendAck() {
    // Bit i of the mask is the tick before the newest by i + 1, which is bit i + 1 of the window.
    const uint32_t mask = static_cast<uint32_t>((applied_bits_ >> 1) & 0xFFFFFFFFull);
    std::vector<uint8_t> bytes;
    Encode(Ack{applied_latest_, mask}, bytes);
    transport_.Send(0, Channel::Unreliable, bytes);
    ++stats_.acks_sent;
}

void ClientReplica::HandleStateBuffer(const TransportEvent& event) {
    StateBufferHeader header{};
    std::span<const uint8_t> payload;
    if (!DecodeStateBuffer(event.data, &header, &payload)) {
        Malformed(event.channel);
        return;
    }
    if (WasApplied(header.tick)) {
        return; // a repeat of a tick that is already in
    }
    if (header.batches_sent > batches_applied_) {
        Hold(header, payload); // its events have not all arrived: they may change what its compact ids mean
        return;
    }
    ApplyBuffer(header.tick, payload);
}

void ClientReplica::Hold(const StateBufferHeader& header, std::span<const uint8_t> payload) {
    ++stats_.buffers_held;
    auto position = std::lower_bound(held_.begin(), held_.end(), header.tick,
                                     [](const HeldBuffer& held, uint64_t tick) { return held.tick < tick; });
    if (position != held_.end() && position->tick == header.tick) {
        return; // the same tick twice: one copy is enough
    }
    held_.insert(position, HeldBuffer{header.tick, header.batches_sent,
                                      std::vector<uint8_t>(payload.begin(), payload.end())});
    if (held_.size() > kMaxHeld) {
        held_.erase(held_.begin()); // the oldest is the least useful
        ++stats_.buffers_dropped;
    }
}

void ClientReplica::ReleaseHeld() {
    std::vector<HeldBuffer> waiting;
    std::vector<HeldBuffer> ready;
    for (HeldBuffer& held : held_) {
        (held.batches_sent <= batches_applied_ ? ready : waiting).push_back(std::move(held));
    }
    held_ = std::move(waiting);
    for (const HeldBuffer& held : ready) {
        if (!WasApplied(held.tick)) {
            ApplyBuffer(held.tick, held.payload);
        }
    }
}

void ClientReplica::ApplyBuffer(uint64_t tick, std::span<const uint8_t> payload) {
    // Every event batch the buffer cites has been applied, so the events up to its tick are in.
    receiver_->AdvanceEvents(tick);
    size_t applied = 0;
    if (receiver_->Apply(payload, tick, &applied) == DecodeStatus::Ok) {
        ++stats_.buffers_applied;
        MarkApplied(tick);
        apply_happened_ = true;
    } else {
        ++stats_.buffers_refused; // not confirmed: the server will send it again
    }
}

} // namespace tessera::net
