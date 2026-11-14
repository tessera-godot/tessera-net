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
    : transport_(transport), schema_(schema), predictor_(schema), interpolator_(schema) {}

void ClientReplica::Poll() {
    if (predictor_.RuleRunning()) {
        return; // a rule that polls would run the rule again, in the middle of a replay
    }
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
    if (predictor_.RuleRunning()) {
        return;
    }
    if (state_ == ClientState::Disconnected) {
        return;
    }
    transport_.Disconnect(0);
    state_ = ClientState::Disconnected;
    ResetVisible();
}

uint32_t ClientReplica::SendInput(std::span<const uint8_t> bytes) {
    if (state_ != ClientState::Connected || predictor_.RuleRunning()) {
        return 0;
    }
    if (bytes.size() > kMaxInputBytes) {
        ++stats_.inputs_refused;
        return 0;
    }
    const uint32_t sequence = next_input_sequence_;
    next_input_sequence_ = NextInputSequence(sequence);
    if (recent_input_count_ == recent_inputs_.size()) {
        // The oldest leaves.
        std::copy(recent_inputs_.begin() + 1, recent_inputs_.end(), recent_inputs_.begin());
        --recent_input_count_;
    }
    InputEntry& entry = recent_inputs_[recent_input_count_++];
    entry = InputEntry{};
    entry.sequence = sequence;
    entry.size = static_cast<uint8_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), entry.bytes.begin());
    Input message;
    message.entries.assign(recent_inputs_.begin(), recent_inputs_.begin() + recent_input_count_);
    std::vector<uint8_t> encoded;
    Encode(message, encoded);
    // If the send fails the connection is going down, and the state will say so.
    transport_.Send(0, Channel::Unreliable, encoded);
    ++stats_.inputs_sent;
    if (predictor_.Status() == PredictionStatus::Active && tick_rate_ != 0) {
        // One step of the movement rule for each input, as long as one tick.
        predictor_.Predict(sequence, bytes, 1.0 / static_cast<double>(tick_rate_));
    }
    return sequence;
}

bool ClientReplica::SetPrediction(const PredictionConfig& config, std::string* error) {
    return predictor_.Configure(config, error);
}

bool ClientReplica::SetPredictionSettings(double smoothing_seconds, double snap_distance,
                                          std::string* error) {
    return predictor_.SetSettings(smoothing_seconds, snap_distance, error);
}

void ClientReplica::ClearPrediction() noexcept {
    predictor_.Clear();
}

void ClientReplica::AdvancePrediction(double seconds) noexcept {
    predictor_.Advance(seconds);
}

PredictionStatus ClientReplica::Prediction() const noexcept {
    return predictor_.Status();
}

bool ClientReplica::RuleRunning() const noexcept {
    return predictor_.RuleRunning();
}

const PredictionStats& ClientReplica::PredictionInfo() const noexcept {
    return predictor_.Stats();
}

uint32_t ClientReplica::PredictionPending() const noexcept {
    return predictor_.Pending();
}

uint32_t ClientReplica::PredictedComponents() const noexcept {
    return predictor_.Components();
}

bool ClientReplica::Predicted(std::span<double> out) const noexcept {
    return predictor_.Displayed(out);
}

bool ClientReplica::SetInterpolation(const InterpolationConfig& config, std::string* error) {
    if (!interpolator_.Configure(config, error)) {
        return false;
    }
    if (receiver_) {
        // Connected: start from what the client holds, the newest state of each visible entity.
        interpolator_.Reset(static_cast<uint32_t>(entity_of_.size()));
        for (uint32_t compact_id = 0; compact_id < entity_of_.size(); ++compact_id) {
            if (entity_of_[compact_id].IsNull()) {
                continue;
            }
            interpolator_.OnEnter(compact_id, entity_of_[compact_id].type());
            if (const uint32_t* words = receiver_->Current(compact_id)) {
                interpolator_.OnRecord(compact_id, receiver_->CurrentTick(compact_id), words);
            }
        }
        if (any_applied_) {
            interpolator_.OnBuffer(applied_latest_);
        }
    }
    return true;
}

void ClientReplica::ClearInterpolation() noexcept {
    interpolator_.Clear();
}

void ClientReplica::AdvanceInterpolation(double seconds) noexcept {
    if (tick_rate_ != 0) {
        interpolator_.Advance(seconds * static_cast<double>(tick_rate_));
    }
}

bool ClientReplica::InterpolationSet() const noexcept {
    return interpolator_.Enabled();
}

bool ClientReplica::Interpolating() const noexcept {
    return interpolator_.Running();
}

const InterpolationConfig& ClientReplica::InterpolationSettings() const noexcept {
    return interpolator_.Config();
}

const InterpolationStats& ClientReplica::InterpolationInfo() const noexcept {
    return interpolator_.Stats();
}

double ClientReplica::DisplayedTick() const noexcept {
    return interpolator_.DisplayTick();
}

double ClientReplica::InterpolationLag() const noexcept {
    return interpolator_.LagTicks();
}

size_t ClientReplica::InterpolationMemoryBytes() const noexcept {
    return interpolator_.MemoryBytes();
}

size_t ClientReplica::InterpolationEntities() const noexcept {
    return interpolator_.InUse();
}

size_t ClientReplica::InterpolationStateCount(EntityId id) const noexcept {
    const auto known = compact_of_.find(id.raw());
    return known == compact_of_.end() ? 0 : interpolator_.StateCount(known->second);
}

uint64_t ClientReplica::InterpolationStateTick(EntityId id, uint32_t index) const noexcept {
    const auto known = compact_of_.find(id.raw());
    return known == compact_of_.end() ? 0 : interpolator_.StateTick(known->second, index);
}

const uint32_t* ClientReplica::DisplayedWords(EntityId id) const noexcept {
    const auto known = compact_of_.find(id.raw());
    return known == compact_of_.end() ? nullptr : interpolator_.HeldWords(known->second);
}

bool ClientReplica::DisplayedValue(EntityId id, uint32_t property_index,
                                   std::span<double> out) const noexcept {
    const auto known = compact_of_.find(id.raw());
    return known != compact_of_.end() && interpolator_.Read(known->second, property_index, out);
}

uint64_t ClientReplica::StateTickOf(EntityId id) const noexcept {
    if (!receiver_) {
        return 0;
    }
    const auto known = compact_of_.find(id.raw());
    return known == compact_of_.end() ? 0 : receiver_->CurrentTick(known->second);
}

// An entity that is no longer visible has no state to predict from: what was predicted for it is dropped, and
// the prediction waits until it is back.
void ClientReplica::ForgetPredictionOf(EntityId entity) noexcept {
    if (predictor_.Status() != PredictionStatus::Off && predictor_.Config().entity == entity) {
        predictor_.Reset();
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
    held_.clear();
    receiver_.reset();
    entity_of_.clear();
    compact_of_.clear();
    predictor_.Reset();
    interpolator_.Reset(0);
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
    tick_rate_ = hello.tick_rate;
    predictor_.Reset();
    interpolator_.Reset(hello.max_visible);
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
            interpolator_.OnEnter(compact_id, event.id.type());
            // The entity may have been visible under another id, and the id may have belonged to another
            // entity.
            // (Under the same compact id it was just started fresh by the calls above: nothing to leave.)
            if (const auto known = compact_of_.find(event.id.raw());
                known != compact_of_.end() && known->second != compact_id) {
                receiver_->OnLeave(known->second);
                interpolator_.OnLeave(known->second);
                entity_of_[known->second] = EntityId::Null();
                compact_of_.erase(known);
                ForgetPredictionOf(event.id);
            }
            if (!entity_of_[compact_id].IsNull()) {
                compact_of_.erase(entity_of_[compact_id].raw());
                ForgetPredictionOf(entity_of_[compact_id]);
            }
            entity_of_[compact_id] = event.id;
            compact_of_[event.id.raw()] = compact_id;
        } else {
            receiver_->OnLeave(compact_id);
            interpolator_.OnLeave(compact_id);
            if (!entity_of_[compact_id].IsNull()) {
                compact_of_.erase(entity_of_[compact_id].raw());
                ForgetPredictionOf(entity_of_[compact_id]);
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
    ApplyBuffer(header.tick, header.last_input_sequence, payload);
}

void ClientReplica::Hold(const StateBufferHeader& header, std::span<const uint8_t> payload) {
    ++stats_.buffers_held;
    auto position = std::lower_bound(held_.begin(), held_.end(), header.tick,
                                     [](const HeldBuffer& held, uint64_t tick) { return held.tick < tick; });
    if (position != held_.end() && position->tick == header.tick) {
        return; // the same tick twice: one copy is enough
    }
    held_.insert(position, HeldBuffer{header.tick, header.batches_sent, header.last_input_sequence,
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
            ApplyBuffer(held.tick, held.last_input_sequence, held.payload);
        }
    }
}

void ClientReplica::ApplyBuffer(uint64_t tick, uint32_t last_input_sequence,
                                std::span<const uint8_t> payload) {
    // Every event batch the buffer cites has been applied, so the events up to its tick are in.
    receiver_->AdvanceEvents(tick);
    size_t applied = 0;
    IRecordObserver* observer = interpolator_.Enabled() ? &interpolator_ : nullptr;
    if (receiver_->Apply(payload, tick, &applied, observer) == DecodeStatus::Ok) {
        ++stats_.buffers_applied;
        MarkApplied(tick);
        apply_happened_ = true;
        // The clock of the interpolation sees the tick before the prediction replays anything, so that
        // whatever the movement rule reads of it is the same as outside the rule.
        interpolator_.OnBuffer(tick);
        if (predictor_.Status() != PredictionStatus::Off && tick_rate_ != 0) {
            // Whether or not the buffer has a record of the player's entity, it says which inputs the state
            // reflects. A buffer that is older than one already used is ignored by the predictor.
            predictor_.Reconcile(tick, WordsOf(predictor_.Config().entity), last_input_sequence,
                                 1.0 / static_cast<double>(tick_rate_));
        }
    } else {
        ++stats_.buffers_refused; // not confirmed: the server will send it again
    }
}

} // namespace tessera::net
