// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/replication/bit_stream.hpp"

namespace tessera::net {

namespace {

constexpr uint64_t LowMask(uint32_t count) noexcept {
    return count >= 64 ? ~0ull : ((1ull << count) - 1);
}

// A 64-bit value needs at most 22 chunks of 3 bits.
constexpr uint32_t kMaxVarChunks = 22;

} // namespace

void BitWriter::WriteBits(uint64_t value, uint32_t count) {
    if (count > 56) {
        // Keep `pending_` (up to 7 bits) plus the new bits inside 64 bits.
        WriteBits(value, 32);
        WriteBits(value >> 32, count - 32);
        return;
    }
    value &= LowMask(count);
    pending_ |= value << pending_bits_;
    pending_bits_ += count;
    bit_count_ += count;
    while (pending_bits_ >= 8) {
        out_.push_back(static_cast<uint8_t>(pending_ & 0xFF));
        pending_ >>= 8;
        pending_bits_ -= 8;
    }
}

void BitWriter::WriteVarUInt(uint64_t value) {
    do {
        const uint64_t chunk = value & 7u;
        value >>= 3;
        WriteBits(chunk | (value != 0 ? 8u : 0u), 4);
    } while (value != 0);
}

void BitWriter::WriteVarInt(int64_t value) {
    WriteVarUInt((static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63));
}

void BitWriter::Finish() {
    if (pending_bits_ > 0) {
        out_.push_back(static_cast<uint8_t>(pending_ & 0xFF));
        pending_ = 0;
        pending_bits_ = 0;
    }
}

uint64_t BitReader::ReadBits(uint32_t count) {
    if (!ok_) {
        return 0;
    }
    if (count > BitsLeft()) {
        ok_ = false;
        return 0;
    }
    if (count > 56) {
        const uint64_t low = ReadBits(32);
        const uint64_t high = ReadBits(count - 32);
        return low | (high << 32);
    }
    const uint64_t first_byte = position_ >> 3;
    const uint32_t shift = static_cast<uint32_t>(position_ & 7);
    const uint32_t bytes = (shift + count + 7) / 8;
    uint64_t gathered = 0;
    for (uint32_t i = 0; i < bytes; ++i) {
        gathered |= static_cast<uint64_t>(data_[first_byte + i]) << (8 * i);
    }
    position_ += count;
    return (gathered >> shift) & LowMask(count);
}

uint64_t BitReader::ReadVarUInt() {
    uint64_t value = 0;
    for (uint32_t chunk = 0; chunk < kMaxVarChunks; ++chunk) {
        const uint64_t bits = ReadBits(4);
        if (!ok_) {
            return 0;
        }
        const uint32_t shift = chunk * 3;
        const uint64_t payload = bits & 7u;
        if (shift >= 63 && payload > (shift == 63 ? 1u : 0u)) {
            ok_ = false; // would not fit in 64 bits
            return 0;
        }
        value |= payload << shift;
        if ((bits & 8u) == 0) {
            return value;
        }
    }
    ok_ = false; // the last allowed chunk still asked for more
    return 0;
}

int64_t BitReader::ReadVarInt() {
    const uint64_t zigzag = ReadVarUInt();
    return static_cast<int64_t>((zigzag >> 1) ^ (~(zigzag & 1) + 1));
}

} // namespace tessera::net
