// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/entity_id.hpp"
#include "tessera/net/protocol.hpp"
#include "tessera/net/replication/property_schema.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace tessera::net {

/// The movement rule of the game, written once for the client (the server has its own copy). It changes
/// `state` in place to what it becomes after one step of `dt_seconds` with `input`, and returns false if it
/// could not. `state` holds the components of the predicted properties in the order of
/// `PredictionConfig::properties`: a position takes 3 (x, y, z), an angle 1 (radians), an integer 1 and an
/// enum 1.
using PredictionRule =
    std::function<bool(std::span<double> state, std::span<const uint8_t> input, double dt_seconds)>;

struct PredictionConfig {
    /// The entity the player controls. Only this one is predicted.
    EntityId entity = EntityId::Null();
    /// The properties of that entity's type to predict, by index in its schema (an index may appear once; bit
    /// masks cannot be predicted).
    std::vector<uint32_t> properties;
    PredictionRule rule;
    /// How long, in seconds (0 to 5), a difference between the prediction and the server takes to fade away.
    double smoothing_seconds = 0.1;
    /// A difference above this many world units is not faded: the shown position jumps to the corrected one.
    double snap_distance = 2.0;
};

enum class PredictionStatus : uint8_t {
    Off,     ///< Nothing is configured.
    Waiting, ///< Configured, but the entity has no state yet.
    Active,  ///< Predicting.
    Paused,  ///< Too many inputs waited for a confirmation; it resumes with the next state of the server.
    Failed,  ///< The rule failed; it stays off until the prediction is configured again.
};

struct PredictionStats {
    uint64_t reconciliations = 0;  ///< States of the server the prediction was corrected against.
    uint64_t snaps = 0;            ///< Corrections that jumped instead of fading.
    uint64_t rule_failures = 0;    ///< Times the rule failed (it is then off).
    uint64_t history_overflows = 0; ///< Times too many inputs waited for a confirmation.
    double last_error = 0.0;       ///< The distance between what was shown and the corrected state.
};

/// What the player's client believes about its own entity before the server has said so: the state after the
/// player's inputs, kept up to date with the movement rule, and corrected whenever a state of the server
/// arrives. It does not read a clock or the network; `ClientReplica` feeds it.
class Predictor {
public:
    /// How many inputs may wait for the server to confirm them. At 30 inputs a second that is a little over 2
    /// seconds; a prediction that falls behind further than this pauses itself.
    static constexpr uint32_t kHistoryCapacity = 64;

    /// The schema must outlive the predictor.
    explicit Predictor(const ReplicationSchema& schema);

    /// Starts predicting `config.entity`. Returns false, changing nothing and saying why in `error` when
    /// given, for a configuration that cannot work. The prediction then waits for the first state.
    bool Configure(const PredictionConfig& config, std::string* error);

    /// Forgets the configuration and everything predicted.
    void Clear() noexcept;

    /// Forgets what was predicted (the state, the inputs, the pending correction) and keeps the
    /// configuration: for a connection that ended.
    void Reset() noexcept;

    /// Applies the rule for one input of the player. Returns false, doing nothing, unless the prediction is
    /// `Active`.
    bool Predict(uint32_t sequence, std::span<const uint8_t> input, double dt_seconds);

    /// A state of the server arrived: `tick` is its tick, `entity_words` the quantized state of the entity
    /// (or nullptr if the entity has none) and `confirmed_sequence` the newest input that state reflects (0:
    /// none). Drops the inputs the server has used, starts from the server's state and applies the rest. A
    /// tick that is not newer than the last one used is ignored. Returns true if the prediction was
    /// corrected.
    bool Reconcile(uint64_t tick, const uint32_t* entity_words, uint32_t confirmed_sequence,
                   double dt_seconds);

    PredictionStatus Status() const noexcept {
        return status_;
    }
    /// How many numbers `Displayed` writes.
    uint32_t Components() const noexcept {
        return components_;
    }
    /// The predicted state, to show: writes `Components()` numbers. False, writing nothing, unless `Active`.
    bool Displayed(std::span<double> out) const noexcept;
    const PredictionStats& Stats() const noexcept {
        return stats_;
    }
    const PredictionConfig& Config() const noexcept {
        return config_;
    }
    /// How many inputs wait for a confirmation.
    uint32_t Pending() const noexcept {
        return pending_;
    }

private:
    struct Slot {
        PropertyKind kind;
        const PropertyDescriptor* descriptor;
        uint32_t offset; // of the first component
    };

    void ReadState(const uint32_t* entity_words, std::span<double> out) const noexcept;
    bool ApplyRule(std::span<double> state, std::span<const uint8_t> input, double dt_seconds) noexcept;
    void Fail() noexcept;
    void DropConfirmed(uint32_t confirmed_sequence) noexcept;
    void ClearState() noexcept;

    const ReplicationSchema& schema_;
    PredictionConfig config_;
    PredictionStatus status_ = PredictionStatus::Off;
    PredictionStats stats_;
    std::vector<Slot> slots_;
    uint32_t components_ = 0;
    std::vector<double> state_; // the predicted state
    std::vector<double> work_;  // scratch for a reconciliation
    std::array<InputEntry, kHistoryCapacity> history_{}; // the oldest first, from `head_`
    uint32_t head_ = 0;
    uint32_t pending_ = 0;
    bool any_tick_ = false;
    uint64_t last_tick_ = 0;
};

} // namespace tessera::net
