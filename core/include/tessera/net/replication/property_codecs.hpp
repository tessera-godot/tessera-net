// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/replication/bit_stream.hpp"
#include "tessera/net/replication/property_schema.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::net {

// Every replicated property is kept as a few 32-bit words of quantized state (`PropertyDescriptor::words`).
// Two states are equal when their words are equal, so comparing never needs the original floats.

/// Reads the property of the entity in `slot` from a page's columns (`columns` points at the page's columns
/// struct) and writes its quantized words. Out-of-range values are limited to the nearest allowed value, NaN
/// becomes the property's zero value (the centre of a position, angle 0, the minimum of an integer), and an
/// infinite angle becomes 0.
void QuantizeProperty(const PropertyDescriptor& descriptor, const std::byte* columns, uint32_t slot,
                      uint32_t* words) noexcept;

/// Writes the property in full, with no reference to an earlier state.
void WriteAbsolute(const PropertyDescriptor& descriptor, const uint32_t* words, BitWriter& writer);

/// Reads a property written by `WriteAbsolute`. Returns false, leaving `words` untouched, if the data ends
/// early or holds a value outside the declared range.
bool ReadAbsolute(const PropertyDescriptor& descriptor, BitReader& reader, uint32_t* words);

/// Writes `current` relative to `base`: one flag bit, then whichever is smaller of the difference and the
/// full value (the difference wins a tie). `base` and `current` differ in at least one word.
void WriteDelta(const PropertyDescriptor& descriptor, const uint32_t* base, const uint32_t* current,
                BitWriter& writer);

/// Reads a property written by `WriteDelta`, given the same `base`. Returns false, leaving `words` untouched,
/// if the data ends early or the result would fall outside the declared range.
bool ReadDelta(const PropertyDescriptor& descriptor, BitReader& reader, const uint32_t* base,
               uint32_t* words);

/// The three coordinates a quantized position stands for (the centre of its step).
void DequantizePosition(const PropertyDescriptor& descriptor, const uint32_t* words, double out[3]) noexcept;

/// The angle in radians, 0 up to just under 2 pi, that a quantized angle stands for.
double DequantizeAngle(const PropertyDescriptor& descriptor, const uint32_t* words) noexcept;

/// The value of a quantized integer property.
int64_t IntegerValue(const PropertyDescriptor& descriptor, const uint32_t* words) noexcept;

} // namespace tessera::net
