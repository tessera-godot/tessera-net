// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/entity_id.hpp"
#include "tessera/net/interpolator.hpp"
#include "tessera/net/predictor.hpp"
#include "tessera/net/protocol.hpp"
#include "tessera/net/replication/property_schema.hpp"
#include "tessera/net/replication/receiver_state.hpp"
#include "tessera/net/transport/transport.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
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
    uint64_t inputs_sent = 0;        ///< Inputs that were given a sequence number and sent.
    uint64_t inputs_refused = 0;     ///< Inputs `SendInput` refused for being larger than `kMaxInputBytes`.
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

    /// Reads what the transport has, and acts on it. Does nothing when called from inside the prediction
    /// rule.
    void Poll();

    /// Leaves the server: closes the connection and forgets what the client knew. Harmless if already left.
    /// Does nothing when called from inside the prediction rule.
    void Disconnect();

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
    /// Sends one input of the player to the server and returns the sequence number it was given: 1, 2, 3, ...
    /// (after 0xFFFFFFFF it starts again at 1). The message, on the unreliable channel, carries this input
    /// and the up to 3 before it, so an input that is lost is usually covered by the next message; nothing
    /// tells the client whether the server got it. What the bytes mean is up to the game; they must not be
    /// more than `kMaxInputBytes` (an empty input is fine). Returns 0 and sends nothing when the client is
    /// not `Connected`, when called from inside the prediction rule, and, counting it in
    /// `Stats().inputs_refused`, when the input is too large. Safe to call in any state.
    uint32_t SendInput(std::span<const uint8_t> bytes);

    /// The tick rate the server announced (0 before the client is connected).
    uint32_t TickRate() const noexcept {
        return tick_rate_;
    }

    /// Starts predicting the player's entity: from now on `SendInput` applies `config.rule` to the predicted
    /// state, and each state buffer of the server corrects it (see `Predictor`). Returns false, changing
    /// nothing and saying why in `error` when given, for a configuration that cannot work. Safe to call in
    /// any state; the prediction begins when the next state buffer is applied.
    bool SetPrediction(const PredictionConfig& config, std::string* error = nullptr);
    /// Changes the smoothing time and the snap distance of the prediction that is set, without restarting
    /// it. False, changing nothing, outside the ranges of `PredictionConfig` or with no prediction set.
    bool SetPredictionSettings(double smoothing_seconds, double snap_distance, std::string* error = nullptr);
    /// Stops predicting and forgets the configuration.
    void ClearPrediction() noexcept;
    /// Lets the correction of the prediction slide forward by `seconds`; call it with the time that passed.
    void AdvancePrediction(double seconds) noexcept;
    PredictionStatus Prediction() const noexcept;
    /// True while the prediction rule is running. The rule must not call back into the client: `Poll`,
    /// `Disconnect`, `SendInput` and the calls that change the prediction refuse to act until it returns.
    bool RuleRunning() const noexcept;
    const PredictionStats& PredictionInfo() const noexcept;
    /// How many inputs are waiting for the server to confirm them.
    uint32_t PredictionPending() const noexcept;
    /// How many numbers `Predicted` writes: the components of the predicted properties.
    uint32_t PredictedComponents() const noexcept;
    /// The predicted state to show, `PredictedComponents()` numbers (see `PredictionRule`). False, writing
    /// nothing, unless the prediction is active.
    bool Predicted(std::span<double> out) const noexcept;

    /// Starts showing the entities a little in the past, smoothed between the states received on either side
    /// of the time shown (see `Interpolator`). Returns false, changing nothing and saying why in `error` when
    /// given, for a configuration that cannot work. Safe to call in any state; connected, it starts from the
    /// states the client already holds. The configuration stays through a disconnect and a reconnection,
    /// while the states do not.
    bool SetInterpolation(const InterpolationConfig& config, std::string* error = nullptr);
    /// Stops interpolating and forgets the configuration.
    void ClearInterpolation() noexcept;
    /// Lets the time shown advance by `seconds` (at the tick rate of the server); call it with the time that
    /// passed. Nothing happens before the hello or for time that is negative or not finite.
    void AdvanceInterpolation(double seconds) noexcept;
    /// Whether interpolation is configured, and whether its clock has started (a state buffer was applied).
    bool InterpolationSet() const noexcept;
    bool Interpolating() const noexcept;
    const InterpolationConfig& InterpolationSettings() const noexcept;
    const InterpolationStats& InterpolationInfo() const noexcept;
    /// The tick being shown, and how far behind the newest applied tick it is, in ticks.
    double DisplayedTick() const noexcept;
    double InterpolationLag() const noexcept;
    /// The memory the interpolation holds now; 0 when it is not configured.
    size_t InterpolationMemoryBytes() const noexcept;
    /// How many entities hold room for interpolation states now (the visible ones, while it is on).
    size_t InterpolationEntities() const noexcept;
    /// How many states are kept for an entity, and the tick of one of them (0 is the oldest).
    size_t InterpolationStateCount(EntityId id) const noexcept;
    uint64_t InterpolationStateTick(EntityId id, uint32_t index) const noexcept;
    /// The quantized state of a visible entity as it was at the time shown: the newest with a tick not after
    /// it (see `Interpolator::HeldWords`). Nullptr when the entity is not visible, has no state, or
    /// interpolation is off or has not started. Valid until the next `Poll` or call that changes the
    /// interpolation.
    const uint32_t* DisplayedWords(EntityId id) const noexcept;
    /// The position (3 numbers) or angle (1 number) of property `property_index` of the entity at the time
    /// shown, blended between two states (see `Interpolator::Read`). False, writing nothing, for any other
    /// kind of property or when `DisplayedWords` would be nullptr.
    bool DisplayedValue(EntityId id, uint32_t property_index, std::span<double> out) const noexcept;
    /// The tick of the newest state of a visible entity (the one `WordsOf` gives); 0 if there is none.
    uint64_t StateTickOf(EntityId id) const noexcept;

    /// How many entities this client can see.
    size_t VisibleCount() const noexcept {
        return compact_of_.size();
    }
    const ClientStats& Stats() const noexcept {
        return stats_;
    }

    /// The newest tick of which a state buffer has been applied (0 before any).
    uint64_t LatestTick() const noexcept {
        return applied_latest_;
    }

    /// The newest quantized state of a visible entity (`words` of its type), or nullptr if it is not visible
    /// or no state has arrived yet. Use the schema to read the values.
    const uint32_t* WordsOf(EntityId id) const noexcept;

    /// Calls `fn(EntityId, const uint32_t* words)` for every visible entity that has state.
    template <class F> void ForEachEntity(F&& fn) const {
        if (!receiver_) {
            return;
        }
        for (uint32_t compact_id = 0; compact_id < entity_of_.size(); ++compact_id) {
            if (entity_of_[compact_id].IsNull()) {
                continue;
            }
            if (const uint32_t* words = receiver_->Current(compact_id)) {
                fn(entity_of_[compact_id], words);
            }
        }
    }

private:
    void HandleMessage(const TransportEvent& event);
    void HandleServerHello(const ServerHello& hello);
    void HandleEventBatch(const EventBatch& batch);
    void HandleStateBuffer(const TransportEvent& event);
    void Hold(const StateBufferHeader& header, std::span<const uint8_t> payload);
    void ReleaseHeld();
    void ApplyBuffer(uint64_t tick, uint32_t last_input_sequence, std::span<const uint8_t> payload);
    void ForgetPredictionOf(EntityId entity) noexcept;
    bool WasApplied(uint64_t tick) const noexcept;
    void MarkApplied(uint64_t tick) noexcept;
    void SendAck();
    void Malformed(Channel channel);
    void Fail();
    void ResetVisible();

    ITransport& transport_;
    const ReplicationSchema& schema_;
    ClientState state_ = ClientState::Connecting;
    RejectReason rejection_ = RejectReason::None;
    uint32_t connection_id_ = 0;
    uint32_t tick_rate_ = 0;
    Predictor predictor_;
    Interpolator interpolator_;
    std::unique_ptr<ReceiverState> receiver_;
    uint64_t batches_applied_ = 0;
    std::vector<EntityId> entity_of_;                   // by compact id; Null when the id is free
    std::unordered_map<uint64_t, uint32_t> compact_of_; // entity -> compact id
    // State buffers that cite event batches that have not been applied yet, ascending by tick.
    struct HeldBuffer {
        uint64_t tick;
        uint64_t batches_sent;
        uint32_t last_input_sequence;
        std::vector<uint8_t> payload;
    };
    static constexpr size_t kMaxHeld = 64;
    std::vector<HeldBuffer> held_;
    bool any_applied_ = false;
    uint64_t applied_latest_ = 0;
    uint64_t applied_bits_ = 0;   // bit i: the tick applied_latest_ - i was applied
    bool apply_happened_ = false; // a buffer was applied since the last ack
    std::array<InputEntry, kMaxInputsPerMessage> recent_inputs_{}; // the newest inputs, the oldest first
    uint32_t recent_input_count_ = 0;
    uint32_t next_input_sequence_ = 1;
    ClientStats stats_;
    std::vector<TransportEvent> events_; // scratch
};

} // namespace tessera::net
