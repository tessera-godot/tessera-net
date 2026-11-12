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

    /// The entities that have a state, by increasing id.
    std::vector<uint64_t> Entities() const;
    std::optional<Value> Property(uint64_t entity, std::string_view name) const;
    std::vector<EntityView> Snapshot() const;

private:
    void Close() noexcept;
    bool IsKnown(uint64_t entity) const noexcept;

    TransportFactory factory_;
    std::shared_ptr<const BuiltSchema> schema_;
    std::unique_ptr<net::ITransport> transport_;
    std::unique_ptr<net::ClientReplica> replica_;
    std::vector<uint64_t> known_; // the entities with a state at the last poll, sorted
    SessionState previous_ = SessionState::Disconnected;
    uint64_t generation_ = 0;
    bool polling_ = false;
};

} // namespace tessera::ext
