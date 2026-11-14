// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "godot_warnings.hpp"
#include "tessera/ext/client_session.hpp"
#include "tessera_schema.hpp"

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <chrono>

namespace godot {

// A connection to a Tessera server, seen from GDScript. Call poll() every frame: nothing is read, and no signal
// is sent, until you do. All the calls are safe in any state; asking for something the client does not have
// gives an empty answer, not an error.
TESSERA_GODOT_CLASS_BEGIN
class TesseraClient : public RefCounted {
    GDCLASS(TesseraClient, RefCounted)

public:
    // The order matches tessera::ext::SessionState.
    enum State { CONNECTING, HANDSHAKING, CONNECTED, REJECTED, DISCONNECTED };

    TesseraClient();
    ~TesseraClient();

    // Starts connecting. Returns OK, or ERR_INVALID_PARAMETER with the reason in get_last_error(): no valid schema,
    // a port outside 1 to 65535, or a connection that could not be opened. A server that refuses, or is not
    // there, shows up later, as the state.
    int connect_to(const String& host, int port);
    // Leaves the server. Raises no signal: the caller knows. Harmless if repeated or if nothing is connected.
    void disconnect_from_server();
    // Reads what arrived and sends the signals for it. A call from inside one of those signals does nothing.
    void poll();

    // Sends one input of the player to the server, as bytes the game defines (at most 16; an empty array is
    // fine). Returns its sequence number, 1, 2, 3, ..., or 0 when nothing was sent: the client is not
    // connected, or the input is too large. Safe in any state, and from inside a signal.
    int send_input(const PackedByteArray& bytes);
    // The id the server gave this client, or -1 while it is not CONNECTED. Safe in any state.
    int get_connection_id() const;

    // Predicts the player's own entity. `rule` is the movement rule of the game, a Callable that takes
    // (state: Dictionary, input: PackedByteArray, dt: float) and returns a Dictionary: `state` has the value
    // of each predicted property by name (a position is a Vector3, an angle a float, an integer an int), and
    // the result holds the values after one step of `dt` seconds with that input. A name left out keeps its
    // value; a name that is not predicted is ignored. A rule that does not return a Dictionary, or gives a
    // value of the wrong type, makes the rule fail, which turns the prediction off with one warning. Call it
    // with the id of the player's entity once it is known; it starts when the next state buffer arrives, so
    // the game sends an input on every tick (zeros included) and the prediction begins within a round trip.
    // The rule must not call back into this client (send_input, poll, set_prediction, clear_prediction,
    // connect_to, disconnect_from_server): while it runs those calls are refused, with the reason in
    // get_last_error(). Returns OK, or ERR_INVALID_PARAMETER with the reason in get_last_error() (ERR_BUSY if
    // called from inside the rule). Safe in any state.
    int set_prediction(int64_t entity, const PackedStringArray& properties, const Callable& rule);
    // Stops predicting. A disconnection does the same.
    void clear_prediction();
    // The value to show for a property: the predicted and smoothed one if `entity` is the predicted entity,
    // the prediction is running and `name` is one of the predicted properties; otherwise the same as
    // get_property(), which always gives what the server last said.
    Variant get_predicted_property(int64_t entity, const String& name) const;
    bool is_predicting() const;
    // The state ("off", "waiting", "active", "paused" or "failed") and the counts of the prediction:
    // reconciliations, snaps, rule_failures, history_overflows and last_error (in world units).
    Dictionary get_prediction_stats() const;
    // How long (0 to 5 seconds) a difference between the prediction and the server takes to fade, and the
    // distance (above 0) over which it jumps instead.
    void set_smoothing_seconds(double seconds);
    double get_smoothing_seconds() const;
    void set_snap_distance(double distance);
    double get_snap_distance() const;

    // Shows the entities that move by themselves a little in the past, smoothed between two states of the
    // server. get_interpolated_property() then gives, for a position or an angle, a value in between the two
    // states around a time delay_ticks behind the newest (0 to 30; the server runs at 30 ticks a second, so 2
    // is about 66 ms), and for anything else the value in force at that time. A state that is late holds the
    // entity where it is until it arrives: nothing is guessed ahead. Two states more than snap_distance
    // apart (a teleport), or more than 8 ticks apart (a long silence), are not blended: the entity stays and
    // moves to the new one at its tick. Set it before connecting or later; it stays through a reconnection.
    // Returns OK, or ERR_INVALID_PARAMETER with the reason in get_last_error() for a delay outside 0 to 30 or
    // a snap_distance that is not above 0. With no arguments it sets the defaults, 2 ticks and 2.5.
    // The time moves with poll(), by the time that passed since the last call (at most a quarter of a
    // second).
    // None of these calls is refused inside a prediction rule: they do not call back into the game.
    int set_interpolation(int delay_ticks, double snap_distance);
    // Stops interpolating and forgets the settings. Harmless in any state.
    void clear_interpolation();
    // The value to show for a property of an entity: for the entity that is predicted (see set_prediction),
    // whatever get_predicted_property() gives; with interpolation running, the interpolated value; otherwise
    // the same as get_property(), which always gives what the server last said.
    Variant get_interpolated_property(int64_t entity, const String& name) const;
    // True once interpolation is set and a state buffer has arrived since the connection.
    bool is_interpolating() const;
    // The state ("off", "waiting" or "running"), the settings (delay_ticks, snap_distance) and the counts:
    // states, states_ignored, gap_holds, snaps, stalls and resyncs, and lag_ticks, how far behind the newest
    // tick the time shown is.
    Dictionary get_interpolation_stats() const;

    int get_state() const;
    String get_rejection() const;
    String get_last_error() const;
    PackedInt64Array get_entities() const;
    Variant get_property(int64_t entity, const String& name) const;
    Dictionary get_snapshot() const;

    void set_schema(const Ref<TesseraSchema>& schema);
    Ref<TesseraSchema> get_schema() const;

protected:
    static void _bind_methods();

private:
    tessera::ext::ClientSession session_;
    Ref<TesseraSchema> schema_;
    String last_error_;
    bool emitting_ = false;
    double smoothing_seconds_ = 0.1;
    double snap_distance_ = 2.0;
    bool has_last_poll_ = false;
    std::chrono::steady_clock::time_point last_poll_;

    void ReportRuleFailure();
    // True, with the reason in last_error_, while the prediction rule runs: a rule must not call back.
    bool RefusedInRule();
};
TESSERA_GODOT_CLASS_END

} // namespace godot

VARIANT_ENUM_CAST(TesseraClient::State);
