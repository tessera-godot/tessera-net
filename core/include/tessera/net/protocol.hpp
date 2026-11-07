// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/relevancy/relevancy_types.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace tessera::net {

/// The version a client and a server must share. It changes when a message changes in a way an older program
/// cannot read.
inline constexpr uint32_t kProtocolVersion = 1;

// The messages between a server and a client. Every message starts with one byte that says which it is,
// followed by its fields (whole numbers take 4 bits for each 3 bits of value, so small ones are cheap).
//
// ClientHello (client to server, reliable): the version and the hash of the schema.
// ServerHello (server to client, reliable): accepted or why not; if accepted, the client's connection id, the
//     tick rate, how many compact ids it may use and how many ticks of history it keeps.
// EventBatch (server to client, reliable): a number (1, 2, 3, ...), the tick, and the visibility events of
//     that tick.
// StateBuffer (server to client, unreliable): the tick, how many event batches the server had sent by then,
//     and the buffer written by the replication engine, unchanged.
// Ack (client to server, unreliable): the newest tick applied and a mask of the 32 before it.
enum class MessageKind : uint8_t {
    ClientHello = 1,
    ServerHello = 2,
    EventBatch = 3,
    StateBuffer = 4,
    Ack = 5,
};

enum class RejectReason : uint8_t {
    None = 0,
    VersionMismatch = 1,
    SchemaMismatch = 2,
    ServerFull = 3,
};

struct ClientHello {
    uint32_t version;
    uint64_t schema_hash;
};

struct ServerHello {
    bool accepted;
    RejectReason reason; ///< `None` when accepted.
    uint32_t version;
    uint32_t connection_id;
    uint32_t tick_rate;
    uint32_t max_visible;
    uint32_t max_baseline_age_ticks;
};

struct EventBatch {
    uint64_t number; ///< 1 for the first batch a server sends to a client, then 2, 3, ...
    uint64_t tick;
    std::vector<VisibilityEvent> events;
};

struct StateBufferHeader {
    uint64_t tick;
    uint64_t batches_sent; ///< How many `EventBatch` messages the server had sent when it wrote this buffer.
};

/// `bit i` set means the tick `latest_tick - 1 - i` was applied as well.
struct Ack {
    uint64_t latest_tick;
    uint32_t mask;
};

/// The kind of a message, or false if it is empty or starts with an unknown byte.
bool PeekKind(std::span<const uint8_t> message, MessageKind* kind);

// Each `Encode` appends one message to `out`. Each `Decode` returns false, leaving its result untouched, for
// a message that is the wrong kind, is truncated, has bytes left over, or holds a value that cannot be right.
void Encode(const ClientHello& message, std::vector<uint8_t>& out);
bool Decode(std::span<const uint8_t> message, ClientHello* result);
void Encode(const ServerHello& message, std::vector<uint8_t>& out);
bool Decode(std::span<const uint8_t> message, ServerHello* result);
void Encode(const EventBatch& message, std::vector<uint8_t>& out);
bool Decode(std::span<const uint8_t> message, EventBatch* result);
void Encode(const Ack& message, std::vector<uint8_t>& out);
bool Decode(std::span<const uint8_t> message, Ack* result);

/// A state buffer carries the replication buffer after its header, as it is.
void EncodeStateBuffer(const StateBufferHeader& header, std::span<const uint8_t> payload,
                       std::vector<uint8_t>& out);
bool DecodeStateBuffer(std::span<const uint8_t> message, StateBufferHeader* header,
                       std::span<const uint8_t>* payload);

/// The ticks a client has confirmed, remembered over a window of 64 ticks behind the newest, so that an `Ack`
/// that repeats ticks already known (the mask makes every `Ack` repeat the 32 before it) confirms each tick
/// once.
class AckWindow {
public:
    /// The ticks this `Ack` confirms for the first time, ascending. A tick more than 63 behind the newest one
    /// seen is ignored.
    std::vector<uint64_t> Add(const Ack& ack);

    /// The newest tick confirmed so far (0 before any).
    uint64_t Latest() const noexcept {
        return latest_;
    }

private:
    bool any_ = false;
    uint64_t latest_ = 0;
    uint64_t seen_ = 0; // bit i: the tick latest_ - i was confirmed
};

} // namespace tessera::net
