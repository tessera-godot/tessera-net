// SPDX-License-Identifier: Apache-2.0
// The client of a Tessera server without anything from Godot: a connection, the state it holds, and the events
// that a script would hear about. The Godot class is a thin layer over this one.
#pragma once

#include "tessera/ext/schema_description.hpp"
#include "tessera/ext/value_conversion.hpp"
#include "tessera/net/client_replica.hpp"
#include "tessera/net/transport/transport.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ext {

enum class SessionState : uint8_t { Connecting, Handshaking, Connected, Rejected, Disconnected };

struct SessionEvent {
    enum class Kind : uint8_t { Connected, Disconnected, Rejected, EntityAppeared, EntityRemoved };

    Kind kind = Kind::Connected;
    uint64_t entity = 0;
    std::string reason; ///< Only for `Rejected`: why the server said no, in words.
};

struct NamedValue {
    std::string name;
    Value value;
};

struct EntityView {
    uint64_t id = 0;
    uint32_t type = 0;
    std::vector<NamedValue> properties;
};

/// The movement rule of the game, for the values a script sees: it changes `state` (the predicted
/// properties, by name; a position is a Vector3, an angle a Float, an integer or a choice an Int) to what it
/// becomes after one step of `dt_seconds` with `input`, and returns false if it could not. It may throw.
using ValueRule =
    std::function<bool(std::vector<NamedValue>& state, std::span<const uint8_t> input, double dt_seconds)>;

/// How a difference between the prediction and the server is shown (see `net::PredictionConfig`).
struct PredictionSettings {
    double smoothing_seconds = 0.1;
    double snap_distance = 2.0;
};

struct PredictionReport {
    net::PredictionStatus status = net::PredictionStatus::Off;
    net::PredictionStats stats;
    std::string failure; ///< Why the rule failed, when `status` is `Failed`.
};

/// Opens the transport for a connection to `host:port`. May throw; the session turns that into an error.
using TransportFactory = std::function<std::unique_ptr<net::ITransport>(const std::string& host, uint16_t port)>;

class ClientSession {
public:
    explicit ClientSession(TransportFactory factory);
    ~ClientSession();
    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    /// Starts a connection, closing the one there is (without events). Returns false, with the reason in
    /// `error`, if the schema is missing, the port is outside 1 to 65535, or the transport cannot open; the
    /// session is then disconnected. The session shares the schema: it stays alive as long as the session needs it.
    bool Connect(std::shared_ptr<const BuiltSchema> schema, const std::string& host, int port,
                 std::string& error);

    /// Closes the connection. Harmless in any state and when repeated; raises no events.
    void Disconnect() noexcept;

    /// Reads what arrived and appends the events it caused to `out`, in this order: Connected, EntityRemoved
    /// (by increasing id), EntityAppeared (by increasing id), then Rejected or Disconnected. A call from inside
    /// a call (a transport that calls back) does nothing.
    void Poll(std::vector<SessionEvent>& out);

    SessionState State() const noexcept;
    std::string RejectionText() const;

    /// Grows with every successful `Connect` and every `Disconnect` that ended a connection, so that a caller
    /// that hands out events can tell that its own handlers changed the connection under it.
    uint64_t Generation() const noexcept {
        return generation_;
    }

    /// Sends one input of the player and returns its sequence number (1, 2, 3, ...). Returns 0, and sends
    /// nothing, when the session is not connected, when the input is larger than `net::kMaxInputBytes`, or if
    /// anything goes wrong. Never throws, and may be called from anywhere, a transport callback included.
    uint32_t SendInput(std::span<const uint8_t> bytes) noexcept;

    /// The id the server gave this client, or -1 while the session is not `Connected`.
    int64_t ConnectionId() const noexcept;

    /// Starts predicting the player's `entity`: `properties` are the names of what `rule` moves (positions,
    /// angles, integers and choices; flags and masks cannot be predicted). From then on `SendInput` applies
    /// the rule at once and each state of the server corrects it, and `PredictedProperty` reads the result. It
    /// begins with the first state that arrives after this call. Returns false, with the reason in `error`,
    /// when there is no connection, the entity or a name is not valid, the rule is empty or the settings are
    /// out of range. A rule that fails ends the prediction (see `Prediction`); the session goes on.
    bool SetPrediction(uint64_t entity, const std::vector<std::string>& properties, ValueRule rule,
                       const PredictionSettings& settings, std::string& error);
    /// Stops predicting. Harmless in any state; a disconnection does the same.
    void ClearPrediction() noexcept;
    /// Changes the settings of the prediction that is set (and the ones a later `SetPrediction` would use are
    /// not touched). Returns false, with the reason in `error`, outside the ranges or with nothing set.
    bool SetPredictionSettings(const PredictionSettings& settings, std::string& error);
    /// Lets the correction slide forward by `seconds` (call it with the time that passed). Negative or not
    /// finite time does nothing.
    void Advance(double seconds) noexcept;
    /// The value to show for a property: the predicted one if `entity` is the predicted entity, the
    /// prediction is active and `name` is one of the predicted properties; otherwise the same as `Property`.
    std::optional<Value> PredictedProperty(uint64_t entity, std::string_view name) const;
    PredictionReport Prediction() const;

    /// The entities that have a state, by increasing id.
    std::vector<uint64_t> Entities() const;
    std::optional<Value> Property(uint64_t entity, std::string_view name) const;
    std::vector<EntityView> Snapshot() const;

private:
    void Close() noexcept;
    bool IsKnown(uint64_t entity) const noexcept;

    struct PredictionState; // the names the rule works on, and why it failed

    TransportFactory factory_;
    std::shared_ptr<const BuiltSchema> schema_;
    std::unique_ptr<net::ITransport> transport_;
    std::unique_ptr<net::ClientReplica> replica_;
    std::vector<uint64_t> known_; // the entities with a state at the last poll, sorted
    SessionState previous_ = SessionState::Disconnected;
    uint64_t generation_ = 0;
    bool polling_ = false;
    std::shared_ptr<PredictionState> prediction_;
};

} // namespace tessera::ext
