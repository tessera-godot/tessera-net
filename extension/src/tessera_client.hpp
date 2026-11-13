// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "godot_warnings.hpp"
#include "tessera/ext/client_session.hpp"
#include "tessera_schema.hpp"

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

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
};
TESSERA_GODOT_CLASS_END

} // namespace godot

VARIANT_ENUM_CAST(TesseraClient::State);
