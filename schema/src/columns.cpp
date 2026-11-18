// SPDX-License-Identifier: Apache-2.0
#include "tessera/schema/columns.hpp"

#include <limits>

namespace tessera::schema {

namespace {

ColumnSpec Unsigned(uint8_t bytes) {
    switch (bytes) {
    case 1:
        return {"uint8_t", 1};
    case 2:
        return {"uint16_t", 2};
    case 4:
        return {"uint32_t", 4};
    default:
        return {"uint64_t", 8};
    }
}

ColumnSpec Signed(uint8_t bytes) {
    switch (bytes) {
    case 1:
        return {"int8_t", 1};
    case 2:
        return {"int16_t", 2};
    case 4:
        return {"int32_t", 4};
    default:
        return {"int64_t", 8};
    }
}

// The smallest integer type that holds the whole range, as the client side of the schema picks it.
ColumnSpec IntegerColumn(const PropertyDecl& p) {
    if (p.is_signed) {
        if (p.min_value >= -128 && p.max_value <= 127) {
            return Signed(1);
        }
        if (p.min_value >= -32768 && p.max_value <= 32767) {
            return Signed(2);
        }
        if (p.min_value >= std::numeric_limits<int32_t>::min() &&
            p.max_value <= std::numeric_limits<int32_t>::max()) {
            return Signed(4);
        }
        return Signed(8);
    }
    if (p.max_value <= 255) {
        return Unsigned(1);
    }
    if (p.max_value <= 65535) {
        return Unsigned(2);
    }
    return Unsigned(p.max_value <= static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) ? 4 : 8);
}

} // namespace

ColumnSpec ColumnFor(const PropertyDecl& property) {
    switch (property.kind) {
    case Kind::Position:
    case Kind::Angle:
        return {"float", 4};
    case Kind::Integer:
        return IntegerColumn(property);
    case Kind::Flag:
        return Unsigned(1);
    case Kind::Mask:
        return Unsigned(property.bits <= 8 ? 1 : property.bits <= 16 ? 2 : property.bits <= 32 ? 4 : 8);
    case Kind::Choice: {
        const uint32_t largest = property.count - 1; // a count of 0 wraps; the check refuses it later
        return Unsigned(largest <= 255 ? 1 : largest <= 65535 ? 2 : 4);
    }
    }
    return Unsigned(8);
}

} // namespace tessera::schema
