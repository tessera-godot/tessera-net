// SPDX-License-Identifier: Apache-2.0
// Turns the quantized words the client holds into values a script can use.
#pragma once

#include "tessera/ext/schema_description.hpp"
#include "tessera/net/replication/property_schema.hpp"

#include <cstdint>

namespace tessera::ext {

/// A value read from an entity. Which member is meaningful depends on `kind`: `vec` for Vector3, `number` for
/// Float, `integer` for Int, `flag` for Bool.
struct Value {
    enum class Kind : uint8_t { Vector3, Float, Int, Bool };

    Kind kind = Kind::Int;
    double vec[3] = {0.0, 0.0, 0.0};
    double number = 0.0;
    int64_t integer = 0;
    bool flag = false;
};

/// Reads one property of an entity. `entity_words` is the start of the entity's quantized words; the property's
/// own offset is added here.
///   position -> Vector3        angle -> Float, in radians    integer -> Int
///   flag -> Bool               mask -> Int                   choice -> Int
/// A mask is up to 64 bits, kept in a signed 64-bit integer: with all 64 bits in use, the top bit makes the
/// number negative (it is the same bits).
Value ConvertProperty(const net::PropertyDescriptor& descriptor, ValueKind kind,
                      const uint32_t* entity_words) noexcept;

} // namespace tessera::ext
