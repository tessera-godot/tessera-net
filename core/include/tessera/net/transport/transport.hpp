// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tessera::net {

/// Identifies one connection at one end of a transport. A server sees each client under an id that is never
/// used again; a client sees the server as connection 0.
using TransportConnection = uint32_t;

enum class Channel : uint8_t {
    ReliableOrdered, ///< Arrives, in the order it was sent, or the connection fails.
    Unreliable,      ///< May be lost, repeated or arrive out of order.
};

/// Something that happened on a transport, in the order the transport saw it.
struct TransportEvent {
    enum class Kind : uint8_t {
        Connected,    ///< A connection opened.
        Disconnected, ///< The other side closed a connection, or it failed.
        Message,      ///< A message arrived.
    };

    Kind kind;
    TransportConnection connection;
    Channel channel = Channel::ReliableOrdered; ///< Only for `Message`.
    std::vector<uint8_t> data;                  ///< Only for `Message`.
};

struct TransportStats {
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
    uint64_t messages_sent = 0;
    uint64_t messages_received = 0;
    uint64_t messages_lost = 0; ///< Messages this end sent that the network dropped.
};

/// What replication needs from the network, and nothing more: messages of up to a few tens of kilobytes on a
/// reliable or an unreliable channel. Splitting them into packets, putting those on the wire and putting them
/// back together is the transport's job.
///
/// A transport is used from one thread at a time.
class ITransport {
public:
    virtual ~ITransport() = default;

    /// Appends to `out` what happened since the last call.
    virtual void Poll(std::vector<TransportEvent>& out) = 0;

    /// Sends a message. Returns false for a connection that is unknown or closed; true does not mean it will
    /// arrive (an unreliable message may be lost).
    virtual bool Send(TransportConnection connection, Channel channel, std::span<const uint8_t> data) = 0;

    /// Closes a connection. The other side sees `Disconnected` after the messages that were sent before.
    virtual void Disconnect(TransportConnection connection) = 0;

    virtual TransportStats Stats() const noexcept = 0;
};

} // namespace tessera::net
