// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/protocol.hpp"

#include "tessera/net/replication/bit_stream.hpp"

#include <algorithm>

namespace tessera::net {

namespace {

// The smallest an event can be on the wire: its kind (2 bits), the entity id (64) and a compact id (4).
constexpr uint64_t kMinEventBits = 2 + 64 + 4;

void WriteKind(BitWriter& writer, MessageKind kind) {
    writer.WriteBits(static_cast<uint64_t>(kind), 8);
}

bool ReadKind(BitReader& reader, MessageKind expected) {
    return reader.ReadBits(8) == static_cast<uint64_t>(expected) && reader.ok();
}

// A message is whole when everything was read and only the padding of the last byte is left.
bool Finished(const BitReader& reader) {
    return reader.ok() && reader.BitsLeft() < 8;
}

bool ReadU32(BitReader& reader, uint32_t* value) {
    const uint64_t read = reader.ReadVarUInt();
    if (!reader.ok() || read > UINT32_MAX) {
        return false;
    }
    *value = static_cast<uint32_t>(read);
    return true;
}

} // namespace

bool PeekKind(std::span<const uint8_t> message, MessageKind* kind) {
    if (message.empty() || message[0] < 1 || message[0] > 6) {
        return false;
    }
    *kind = static_cast<MessageKind>(message[0]);
    return true;
}

void Encode(const ClientHello& message, std::vector<uint8_t>& out) {
    BitWriter writer(out);
    WriteKind(writer, MessageKind::ClientHello);
    writer.WriteVarUInt(message.version);
    writer.WriteBits(message.schema_hash, 64);
    writer.Finish();
}

bool Decode(std::span<const uint8_t> message, ClientHello* result) {
    BitReader reader(message);
    ClientHello hello{};
    if (!ReadKind(reader, MessageKind::ClientHello) || !ReadU32(reader, &hello.version)) {
        return false;
    }
    hello.schema_hash = reader.ReadBits(64);
    if (!Finished(reader)) {
        return false;
    }
    *result = hello;
    return true;
}

void Encode(const ServerHello& message, std::vector<uint8_t>& out) {
    BitWriter writer(out);
    WriteKind(writer, MessageKind::ServerHello);
    writer.WriteBool(message.accepted);
    writer.WriteBits(static_cast<uint64_t>(message.reason), 2);
    if (message.accepted) {
        writer.WriteVarUInt(message.version);
        writer.WriteVarUInt(message.connection_id);
        writer.WriteVarUInt(message.tick_rate);
        writer.WriteVarUInt(message.max_visible);
        writer.WriteVarUInt(message.max_baseline_age_ticks);
    }
    writer.Finish();
}

bool Decode(std::span<const uint8_t> message, ServerHello* result) {
    BitReader reader(message);
    ServerHello hello{};
    if (!ReadKind(reader, MessageKind::ServerHello)) {
        return false;
    }
    hello.accepted = reader.ReadBool();
    hello.reason = static_cast<RejectReason>(reader.ReadBits(2));
    hello.version = hello.connection_id = hello.tick_rate = hello.max_visible = hello.max_baseline_age_ticks =
        0;
    if (hello.accepted && !(ReadU32(reader, &hello.version) && ReadU32(reader, &hello.connection_id) &&
                            ReadU32(reader, &hello.tick_rate) && ReadU32(reader, &hello.max_visible) &&
                            ReadU32(reader, &hello.max_baseline_age_ticks))) {
        return false;
    }
    if (!Finished(reader)) {
        return false;
    }
    *result = hello;
    return true;
}

void Encode(const EventBatch& message, std::vector<uint8_t>& out) {
    BitWriter writer(out);
    WriteKind(writer, MessageKind::EventBatch);
    writer.WriteVarUInt(message.number);
    writer.WriteVarUInt(message.tick);
    writer.WriteVarUInt(message.events.size());
    for (const VisibilityEvent& event : message.events) {
        writer.WriteBits(static_cast<uint64_t>(event.kind), 2);
        writer.WriteBits(event.id.raw(), 64);
        writer.WriteVarUInt(event.compact_id);
    }
    writer.Finish();
}

bool Decode(std::span<const uint8_t> message, EventBatch* result) {
    BitReader reader(message);
    if (!ReadKind(reader, MessageKind::EventBatch)) {
        return false;
    }
    EventBatch batch;
    batch.number = reader.ReadVarUInt();
    batch.tick = reader.ReadVarUInt();
    const uint64_t count = reader.ReadVarUInt();
    // Whatever the message claims, it cannot hold more events than it has bits for: nothing is allocated for
    // a lie.
    if (!reader.ok() || count > reader.BitsLeft() / kMinEventBits) {
        return false;
    }
    batch.events.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t kind = reader.ReadBits(2);
        const uint64_t raw = reader.ReadBits(64);
        uint32_t compact_id = 0;
        if (!ReadU32(reader, &compact_id) || kind > 2) {
            return false;
        }
        batch.events.push_back(
            VisibilityEvent{static_cast<VisibilityEvent::Kind>(kind), EntityId::FromRaw(raw), compact_id});
    }
    if (!Finished(reader)) {
        return false;
    }
    *result = std::move(batch);
    return true;
}

void Encode(const Ack& message, std::vector<uint8_t>& out) {
    BitWriter writer(out);
    WriteKind(writer, MessageKind::Ack);
    writer.WriteVarUInt(message.latest_tick);
    writer.WriteBits(message.mask, 32);
    writer.Finish();
}

bool Decode(std::span<const uint8_t> message, Ack* result) {
    BitReader reader(message);
    if (!ReadKind(reader, MessageKind::Ack)) {
        return false;
    }
    Ack ack{};
    ack.latest_tick = reader.ReadVarUInt();
    ack.mask = static_cast<uint32_t>(reader.ReadBits(32));
    if (!Finished(reader)) {
        return false;
    }
    *result = ack;
    return true;
}

void EncodeStateBuffer(const StateBufferHeader& header, std::span<const uint8_t> payload,
                       std::vector<uint8_t>& out) {
    BitWriter writer(out);
    WriteKind(writer, MessageKind::StateBuffer);
    writer.WriteVarUInt(header.tick);
    writer.WriteVarUInt(header.batches_sent);
    writer.Finish();
    out.insert(out.end(), payload.begin(), payload.end());
}

bool DecodeStateBuffer(std::span<const uint8_t> message, StateBufferHeader* header,
                       std::span<const uint8_t>* payload) {
    BitReader reader(message);
    if (!ReadKind(reader, MessageKind::StateBuffer)) {
        return false;
    }
    StateBufferHeader read{};
    read.tick = reader.ReadVarUInt();
    read.batches_sent = reader.ReadVarUInt();
    if (!reader.ok()) {
        return false;
    }
    const uint64_t consumed_bits = message.size() * 8ull - reader.BitsLeft();
    *header = read;
    *payload = message.subspan(static_cast<size_t>((consumed_bits + 7) / 8));
    return true;
}

void Encode(const Input& message, std::vector<uint8_t>& out) {
    BitWriter writer(out);
    WriteKind(writer, MessageKind::Input);
    const InputEntry& newest = message.entries.back();
    writer.WriteVarUInt(newest.sequence);
    writer.WriteVarUInt(message.entries.size());
    for (const InputEntry& entry : message.entries) {
        const uint32_t size = std::min<uint32_t>(entry.size, kMaxInputBytes);
        writer.WriteVarUInt(
            static_cast<uint32_t>(newest.sequence - entry.sequence)); // how far behind the newest
        writer.WriteVarUInt(size);
        for (uint32_t i = 0; i < size; ++i) {
            writer.WriteBits(entry.bytes[i], 8);
        }
    }
    writer.Finish();
}

bool Decode(std::span<const uint8_t> message, Input* result) {
    BitReader reader(message);
    uint32_t newest = 0;
    uint32_t count = 0;
    if (!ReadKind(reader, MessageKind::Input) || !ReadU32(reader, &newest) || !ReadU32(reader, &count) ||
        count < 1 || count > kMaxInputsPerMessage) {
        return false;
    }
    Input input;
    input.entries.reserve(count);
    uint64_t previous_back = UINT64_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t back = 0;
        uint32_t size = 0;
        if (!ReadU32(reader, &back) || !ReadU32(reader, &size) || size > kMaxInputBytes ||
            back >= previous_back || (i + 1 == count) != (back == 0)) {
            return false;
        }
        previous_back = back;
        InputEntry entry;
        entry.sequence = newest - back;
        entry.size = static_cast<uint8_t>(size);
        for (uint32_t b = 0; b < size; ++b) {
            entry.bytes[b] = static_cast<uint8_t>(reader.ReadBits(8));
        }
        input.entries.push_back(entry);
    }
    if (!Finished(reader)) {
        return false;
    }
    *result = std::move(input);
    return true;
}

InputVerdict InputWindow::Judge(uint32_t sequence) const noexcept {
    if (!any_) {
        // The first one is new whatever it is, except 0, which is never a sequence number.
        return sequence == 0 ? InputVerdict::Repeated : InputVerdict::New;
    }
    const uint32_t ahead = sequence - latest_; // serial arithmetic: wraps around 2^32
    if (ahead == 0) {
        return InputVerdict::Repeated;
    }
    if (ahead < 0x80000000u) {
        return InputVerdict::New;
    }
    const uint32_t behind = latest_ - sequence; // 1 or more
    if (behind <= 32 && ((seen_ >> (behind - 1)) & 1u) != 0) {
        return InputVerdict::Repeated;
    }
    return InputVerdict::Late;
}

void InputWindow::Accept(uint32_t sequence) noexcept {
    if (Judge(sequence) != InputVerdict::New) {
        return;
    }
    if (any_) {
        const uint32_t shift = sequence - latest_;
        seen_ = shift < 32 ? seen_ << shift : 0;
        if (shift <= 32) {
            seen_ |= 1u << (shift - 1); // the old newest is now `shift` behind
        }
    }
    any_ = true;
    latest_ = sequence;
}

std::vector<uint64_t> AckWindow::Add(const Ack& ack) {
    if (!any_ || ack.latest_tick > latest_) {
        const uint64_t shift = any_ ? ack.latest_tick - latest_ : 0;
        seen_ = shift >= 64 ? 0 : seen_ << shift;
        latest_ = ack.latest_tick;
        any_ = true;
    }
    std::vector<uint64_t> fresh;
    auto confirm = [&](uint64_t tick) {
        const uint64_t offset = latest_ - tick; // tick <= ack.latest_tick <= latest_
        if (offset >= 64 || ((seen_ >> offset) & 1ull) != 0) {
            return;
        }
        seen_ |= 1ull << offset;
        fresh.push_back(tick);
    };
    confirm(ack.latest_tick);
    for (uint32_t i = 0; i < 32; ++i) {
        if (((ack.mask >> i) & 1u) != 0 && ack.latest_tick >= 1ull + i) {
            confirm(ack.latest_tick - 1 - i);
        }
    }
    std::sort(fresh.begin(), fresh.end());
    return fresh;
}

} // namespace tessera::net
