// SPDX-License-Identifier: Apache-2.0
#include "tessera/ext/value_conversion.hpp"

#include "tessera/net/replication/property_codecs.hpp"

namespace tessera::ext {

Value ConvertProperty(const net::PropertyDescriptor& descriptor, ValueKind kind,
                      const uint32_t* entity_words) noexcept {
    const uint32_t* words = entity_words + descriptor.word_offset;
    Value value;
    switch (kind) {
    case ValueKind::Position:
        value.kind = Value::Kind::Vector3;
        net::DequantizePosition(descriptor, words, value.vec);
        break;
    case ValueKind::Angle:
        value.kind = Value::Kind::Float;
        value.number = net::DequantizeAngle(descriptor, words);
        break;
    case ValueKind::Integer:
        value.kind = Value::Kind::Int;
        value.integer = net::IntegerValue(descriptor, words);
        break;
    case ValueKind::Flag:
        value.kind = Value::Kind::Bool;
        value.flag = (words[0] & 1u) != 0;
        break;
    case ValueKind::Mask:
        // Both words, the low one first. With all 64 bits in use the top one makes the number negative.
        value.kind = Value::Kind::Int;
        value.integer = net::IntegerValue(descriptor, words);
        break;
    case ValueKind::Choice:
        // An enum holds its value in one word; IntegerValue only reads the wide kinds.
        value.kind = Value::Kind::Int;
        value.integer = static_cast<int64_t>(words[0]);
        break;
    }
    return value;
}

} // namespace tessera::ext
