// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/replication/property_codecs.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace tessera::net {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

float LoadFloat(const std::byte* columns, const ColumnRef& column, uint32_t slot) noexcept {
    float value;
    std::memcpy(&value, columns + column.offset + static_cast<size_t>(slot) * 4, sizeof(value));
    return value;
}

// Reads one integer element as an unsigned number (zero-extended).
uint64_t LoadUnsigned(const std::byte* columns, const ColumnRef& column, uint32_t slot) noexcept {
    const std::byte* at = columns + column.offset + static_cast<size_t>(slot) * column.element_bytes;
    switch (column.element_bytes) {
    case 1:
        return static_cast<uint8_t>(*at);
    case 2: {
        uint16_t value;
        std::memcpy(&value, at, sizeof(value));
        return value;
    }
    case 4: {
        uint32_t value;
        std::memcpy(&value, at, sizeof(value));
        return value;
    }
    default: {
        uint64_t value;
        std::memcpy(&value, at, sizeof(value));
        return value;
    }
    }
}

// Reads one integer element as a signed number (sign-extended).
int64_t LoadSigned(const std::byte* columns, const ColumnRef& column, uint32_t slot) noexcept {
    const uint64_t raw = LoadUnsigned(columns, column, slot);
    switch (column.element_bytes) {
    case 1:
        return static_cast<int8_t>(raw);
    case 2:
        return static_cast<int16_t>(raw);
    case 4:
        return static_cast<int32_t>(raw);
    default:
        return static_cast<int64_t>(raw);
    }
}

uint32_t QuantizeAxis(float value, const PropertyDescriptor& d) noexcept {
    double x = static_cast<double>(value);
    if (std::isnan(x)) {
        x = 0.0;
    }
    const double range = static_cast<double>(d.range);
    x = std::clamp(x, -range, range);
    const long long ordinal = std::llround((x + range) / static_cast<double>(d.step));
    return static_cast<uint32_t>(std::clamp<long long>(ordinal, 0, static_cast<long long>(d.max_ordinal)));
}

uint32_t QuantizeAngle(float value, const PropertyDescriptor& d) noexcept {
    double x = static_cast<double>(value);
    if (!std::isfinite(x)) {
        return 0;
    }
    double turns = std::fmod(x, kTwoPi) / kTwoPi;
    if (turns < 0.0) {
        turns += 1.0;
    }
    const long long ordinal = std::llround(turns * static_cast<double>(1ull << d.bits));
    return static_cast<uint32_t>(static_cast<uint64_t>(ordinal) & d.max_ordinal); // 2^bits wraps to 0
}

// The signed difference between two angle ordinals the short way round, in [-2^(bits-1), 2^(bits-1)).
int64_t CircularDifference(uint32_t current, uint32_t base, uint32_t bits) noexcept {
    const uint64_t size = 1ull << bits;
    int64_t difference = static_cast<int64_t>((static_cast<uint64_t>(current) + size - base) & (size - 1));
    if (difference >= static_cast<int64_t>(size / 2)) {
        difference -= static_cast<int64_t>(size);
    }
    return difference;
}

// Integers and bit masks keep a 64-bit ordinal in two words, low word first.
uint64_t Join(const uint32_t* words) noexcept {
    return static_cast<uint64_t>(words[0]) | (static_cast<uint64_t>(words[1]) << 32);
}

void Split(uint64_t value, uint32_t* words) noexcept {
    words[0] = static_cast<uint32_t>(value);
    words[1] = static_cast<uint32_t>(value >> 32);
}

bool IsWide(PropertyKind kind) noexcept {
    return kind == PropertyKind::Integer || kind == PropertyKind::Bits;
}

} // namespace

void QuantizeProperty(const PropertyDescriptor& d, const std::byte* columns, uint32_t slot,
                      uint32_t* words) noexcept {
    switch (d.kind) {
    case PropertyKind::Position:
        for (uint32_t axis = 0; axis < 3; ++axis) {
            words[axis] = QuantizeAxis(LoadFloat(columns, d.source[axis], slot), d);
        }
        return;
    case PropertyKind::Angle:
        words[0] = QuantizeAngle(LoadFloat(columns, d.source[0], slot), d);
        return;
    case PropertyKind::Integer: {
        int64_t value;
        if (d.is_signed) {
            value = LoadSigned(columns, d.source[0], slot);
        } else {
            // An unsigned 64-bit value above the largest signed one is simply limited to the maximum.
            const uint64_t raw = LoadUnsigned(columns, d.source[0], slot);
            value = raw > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(raw);
        }
        value = std::clamp(value, d.min_value, d.max_value);
        Split(static_cast<uint64_t>(value) - static_cast<uint64_t>(d.min_value), words);
        return;
    }
    case PropertyKind::Bits:
        Split(LoadUnsigned(columns, d.source[0], slot) & d.max_ordinal, words);
        return;
    case PropertyKind::Enum:
        words[0] = static_cast<uint32_t>(
            std::min<uint64_t>(LoadUnsigned(columns, d.source[0], slot), d.max_ordinal));
        return;
    }
}

void WriteAbsolute(const PropertyDescriptor& d, const uint32_t* words, BitWriter& writer) {
    switch (d.kind) {
    case PropertyKind::Position:
        for (uint32_t axis = 0; axis < 3; ++axis) {
            writer.WriteBits(words[axis], d.absolute_bits);
        }
        return;
    case PropertyKind::Angle:
    case PropertyKind::Enum:
        writer.WriteBits(words[0], d.absolute_bits);
        return;
    case PropertyKind::Integer:
    case PropertyKind::Bits:
        writer.WriteBits(Join(words), d.absolute_bits);
        return;
    }
}

bool ReadAbsolute(const PropertyDescriptor& d, BitReader& reader, uint32_t* words) {
    switch (d.kind) {
    case PropertyKind::Position: {
        uint32_t read[3];
        for (uint32_t axis = 0; axis < 3; ++axis) {
            read[axis] = static_cast<uint32_t>(reader.ReadBits(d.absolute_bits));
        }
        if (!reader.ok() || read[0] > d.max_ordinal || read[1] > d.max_ordinal || read[2] > d.max_ordinal) {
            return false;
        }
        std::copy(read, read + 3, words);
        return true;
    }
    case PropertyKind::Angle:
    case PropertyKind::Enum: {
        const uint64_t read = reader.ReadBits(d.absolute_bits);
        if (!reader.ok() || read > d.max_ordinal) {
            return false;
        }
        words[0] = static_cast<uint32_t>(read);
        return true;
    }
    case PropertyKind::Integer:
    case PropertyKind::Bits: {
        const uint64_t read = reader.ReadBits(d.absolute_bits);
        if (!reader.ok() || read > d.max_ordinal) {
            return false;
        }
        Split(read, words);
        return true;
    }
    }
    return false;
}

void WriteDelta(const PropertyDescriptor& d, const uint32_t* base, const uint32_t* current,
                BitWriter& writer) {
    switch (d.kind) {
    case PropertyKind::Position: {
        int64_t difference[3];
        uint32_t delta_cost = 0;
        for (uint32_t axis = 0; axis < 3; ++axis) {
            difference[axis] = static_cast<int64_t>(current[axis]) - static_cast<int64_t>(base[axis]);
            delta_cost += VarIntBits(difference[axis]);
        }
        if (delta_cost <= 3 * d.absolute_bits) {
            writer.WriteBool(false);
            for (const int64_t step : difference) {
                writer.WriteVarInt(step);
            }
        } else {
            writer.WriteBool(true);
            WriteAbsolute(d, current, writer);
        }
        return;
    }
    case PropertyKind::Angle: {
        const int64_t difference = CircularDifference(current[0], base[0], d.bits);
        if (VarIntBits(difference) <= d.absolute_bits) {
            writer.WriteBool(false);
            writer.WriteVarInt(difference);
        } else {
            writer.WriteBool(true);
            WriteAbsolute(d, current, writer);
        }
        return;
    }
    case PropertyKind::Integer: {
        // Modular subtraction: the receiver adds it back modulo 2^64, so even the widest range is exact.
        const int64_t difference = static_cast<int64_t>(Join(current) - Join(base));
        if (VarIntBits(difference) <= d.absolute_bits) {
            writer.WriteBool(false);
            writer.WriteVarInt(difference);
        } else {
            writer.WriteBool(true);
            WriteAbsolute(d, current, writer);
        }
        return;
    }
    case PropertyKind::Bits: {
        const uint64_t changed = Join(current) ^ Join(base);
        if (VarUIntBits(changed) <= d.absolute_bits) {
            writer.WriteBool(false);
            writer.WriteVarUInt(changed);
        } else {
            writer.WriteBool(true);
            WriteAbsolute(d, current, writer);
        }
        return;
    }
    case PropertyKind::Enum:
        WriteAbsolute(d, current, writer); // a few bits either way: no flag
        return;
    }
}

bool ReadDelta(const PropertyDescriptor& d, BitReader& reader, const uint32_t* base, uint32_t* words) {
    if (d.kind == PropertyKind::Enum) {
        return ReadAbsolute(d, reader, words);
    }
    const bool absolute = reader.ReadBool();
    if (!reader.ok()) {
        return false;
    }
    if (absolute) {
        return ReadAbsolute(d, reader, words);
    }
    switch (d.kind) {
    case PropertyKind::Position: {
        uint32_t result[3];
        for (uint32_t axis = 0; axis < 3; ++axis) {
            const int64_t step = reader.ReadVarInt();
            const int64_t start = static_cast<int64_t>(base[axis]);
            // Written so that nothing overflows, whatever the bits say.
            if (!reader.ok() || step < -start || step > static_cast<int64_t>(d.max_ordinal) - start) {
                return false;
            }
            result[axis] = static_cast<uint32_t>(start + step);
        }
        std::copy(result, result + 3, words);
        return true;
    }
    case PropertyKind::Angle: {
        const int64_t step = reader.ReadVarInt();
        const int64_t half = static_cast<int64_t>(1ull << (d.bits - 1));
        if (!reader.ok() || step < -half || step >= half) {
            return false;
        }
        const uint64_t size = 1ull << d.bits;
        words[0] = static_cast<uint32_t>(
            (static_cast<uint64_t>(base[0]) + size + static_cast<uint64_t>(step)) & (size - 1));
        return true;
    }
    case PropertyKind::Integer: {
        const int64_t step = reader.ReadVarInt();
        const uint64_t result = Join(base) + static_cast<uint64_t>(step);
        if (!reader.ok() || result > d.max_ordinal) {
            return false;
        }
        Split(result, words);
        return true;
    }
    case PropertyKind::Bits: {
        const uint64_t changed = reader.ReadVarUInt();
        const uint64_t result = Join(base) ^ changed;
        if (!reader.ok() || result > d.max_ordinal) {
            return false;
        }
        Split(result, words);
        return true;
    }
    case PropertyKind::Enum:
        break; // handled above
    }
    return false;
}

void DequantizePosition(const PropertyDescriptor& d, const uint32_t* words, double out[3]) noexcept {
    for (uint32_t axis = 0; axis < 3; ++axis) {
        out[axis] =
            static_cast<double>(words[axis]) * static_cast<double>(d.step) - static_cast<double>(d.range);
    }
}

double DequantizeAngle(const PropertyDescriptor& d, const uint32_t* words) noexcept {
    return static_cast<double>(words[0]) / static_cast<double>(1ull << d.bits) * kTwoPi;
}

int64_t IntegerValue(const PropertyDescriptor& d, const uint32_t* words) noexcept {
    assert(IsWide(d.kind));
    return static_cast<int64_t>(static_cast<uint64_t>(d.min_value) + Join(words));
}

} // namespace tessera::net
