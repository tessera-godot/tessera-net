// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tessera::net {

/// Number of bits needed to write `value` with `BitWriter::WriteVarUInt`. Small numbers are cheap: values
/// 0 to 7 take 4 bits, 8 to 63 take 8, 64 to 511 take 12, and so on (4 bits per 3 bits of value).
constexpr uint32_t VarUIntBits(uint64_t value) noexcept {
    const uint32_t length = static_cast<uint32_t>(std::bit_width(value));
    const uint32_t chunks = length == 0 ? 1u : (length + 2u) / 3u;
    return chunks * 4u;
}

/// Number of bits needed to write `value` with `BitWriter::WriteVarInt` (zigzag, then `VarUInt`).
constexpr uint32_t VarIntBits(int64_t value) noexcept {
    const uint64_t zigzag = (static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63);
    return VarUIntBits(zigzag);
}

/// Smallest width in bits that can hold every number from 0 to `max_value`. Zero for `max_value` 0.
constexpr uint32_t BitsFor(uint64_t max_value) noexcept {
    return static_cast<uint32_t>(std::bit_width(max_value));
}

/// Writes bits and variable-size integers after the end of a byte vector. Fields go in the order they are
/// written, from the least significant bit of each byte up. Call `Finish` when done so the last byte is
/// padded with zero bits.
class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) noexcept : out_(out) {}

    /// Writes the lowest `count` bits of `value`. `count` is 0 to 64; higher bits of `value` are ignored.
    void WriteBits(uint64_t value, uint32_t count);

    void WriteBool(bool value) {
        WriteBits(value ? 1u : 0u, 1);
    }

    /// Writes `value` in chunks of 3 bits, each followed by a flag that says whether another chunk follows,
    /// least significant chunk first.
    void WriteVarUInt(uint64_t value);

    /// Writes a signed number as zigzag (0, -1, 1, -2, ... map to 0, 1, 2, 3, ...) and then `WriteVarUInt`.
    void WriteVarInt(int64_t value);

    /// Bits written through this writer, padding not included.
    uint64_t BitCount() const noexcept {
        return bit_count_;
    }

    /// A place in the output that `Rewind` can go back to.
    struct Mark {
        size_t size;
        uint64_t pending;
        uint32_t pending_bits;
        uint64_t bit_count;
    };

    Mark GetMark() const noexcept {
        return Mark{out_.size(), pending_, pending_bits_, bit_count_};
    }

    /// Takes back everything written since `mark`, which must come from this writer and not from before a
    /// `Finish`. The output is as if the bits had never been written.
    void Rewind(const Mark& mark) {
        out_.resize(mark.size);
        pending_ = mark.pending;
        pending_bits_ = mark.pending_bits;
        bit_count_ = mark.bit_count;
    }

    /// Pads the last byte with zero bits. Safe to call more than once.
    void Finish();

private:
    std::vector<uint8_t>& out_;
    uint64_t pending_ = 0; // bits not yet moved to `out_`, fewer than 8 between calls
    uint32_t pending_bits_ = 0;
    uint64_t bit_count_ = 0;
};

/// Reads what `BitWriter` wrote. A read past the end, or a malformed variable-size integer, sets an error
/// flag instead of failing: `ok()` turns false and every later read returns zero, so a caller can read a
/// whole record and check once at the end.
class BitReader {
public:
    explicit BitReader(std::span<const uint8_t> data) noexcept : data_(data) {}

    /// Reads `count` bits (0 to 64).
    uint64_t ReadBits(uint32_t count);
    bool ReadBool() {
        return ReadBits(1) != 0;
    }
    uint64_t ReadVarUInt();
    int64_t ReadVarInt();

    bool ok() const noexcept {
        return ok_;
    }

    /// Bits that are still unread.
    uint64_t BitsLeft() const noexcept {
        return data_.size() * 8ull - position_;
    }

private:
    std::span<const uint8_t> data_;
    uint64_t position_ = 0;
    bool ok_ = true;
};

} // namespace tessera::net
