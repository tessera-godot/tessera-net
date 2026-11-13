// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/relevancy/relevancy_types.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace tessera::net {

/// The version a client and a server must share. It changes when a message changes in a way an older program
/// cannot read.
inline constexpr uint32_t kProtocolVersion = 2;

/// The most inputs one `Input` message carries: the newest and the 3 before it.
inline constexpr uint32_t kMaxInputsPerMessage = 4;
/// The most bytes one input can hold. The game decides what they mean.
inline constexpr uint32_t kMaxInputBytes = 16;

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
// Input (client to server, unreliable): up to 4 inputs, the newest and the 3 before it, each with its
//     sequence number and a block of bytes the game defines.
enum class MessageKind : uint8_t {
    ClientHello = 1,
    ServerHello = 2,
    EventBatch = 3,
    StateBuffer = 4,
    Ack = 5,
    Input = 6,
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

/// One input of a player: its sequence number and the bytes the game defined for it.
struct InputEntry {
    uint32_t sequence = 0;
    uint8_t size = 0;
    std::array<uint8_t, kMaxInputBytes> bytes{};

    /// The `size` bytes of the input (at most `kMaxInputBytes`).
    std::span<const uint8_t> Bytes() const noexcept {
        return {bytes.data(), size < kMaxInputBytes ? size : kMaxInputBytes};
    }
};

/// The inputs a client sends in one message, the oldest first. Sequence numbers rise in serial arithmetic.
struct Input {
    std::vector<InputEntry> entries;
};

/// The sequence number after `sequence`. It is never 0, because 0 means "nothing was sent".
constexpr uint32_t NextInputSequence(uint32_t sequence) noexcept {
    return sequence + 1 == 0 ? 1 : sequence + 1;
}

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
/// Precondition: 1 to 4 entries, in rising order. An entry larger than `kMaxInputBytes` is cut to that size.
void Encode(const Input& message, std::vector<uint8_t>& out);
bool Decode(std::span<const uint8_t> message, Input* result);

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

/// What an `InputWindow` makes of a sequence number.
enum class InputVerdict : uint8_t {
    New,      ///< Newer than every input delivered so far.
    Repeated, ///< Already delivered.
    Late,     ///< Older than the newest one delivered and never delivered itself.
};

/// Remembers which inputs of one player were delivered, so that an input sent in several messages is
/// delivered once and never after a newer one. Sequence numbers are compared in serial arithmetic: a number
/// less than 2^31 ahead of the newest is new, so the count can wrap around 2^32. The window covers the newest
/// sequence and the 32 before it.
class InputWindow {
public:
    /// Judges a sequence number without changing anything.
    InputVerdict Judge(uint32_t sequence) const noexcept;

    /// Marks a sequence number as delivered. It only has an effect for one that `Judge` calls `New`.
    void Accept(uint32_t sequence) noexcept;

    /// The newest sequence delivered (0 before any).
    uint32_t Latest() const noexcept {
        return latest_;
    }

private:
    bool any_ = false;
    uint32_t latest_ = 0;
    uint32_t seen_ = 0; // bit i: the sequence latest_ - 1 - i was delivered
};

} // namespace tessera::net
