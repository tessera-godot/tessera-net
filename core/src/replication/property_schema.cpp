// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/replication/property_schema.hpp"

#include "tessera/net/entity_id.hpp"
#include "tessera/net/replication/bit_stream.hpp"

#include <bit>
#include <cmath>
#include <limits>

namespace tessera::net {

namespace {

constexpr uint64_t kMaxPositionOrdinal = 0x7FFFFFFFull;

bool Fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool IsIntegerElement(uint8_t bytes) {
    return bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8;
}

// Checks one property and fills in its derived fields (everything except `word_offset`).
bool Prepare(PropertyDescriptor& p, size_t index, std::string* error) {
    const std::string where = "property " + std::to_string(index) + ": ";
    switch (p.kind) {
    case PropertyKind::Position: {
        for (const ColumnRef& column : p.source) {
            if (column.element_bytes != 4) {
                return Fail(error, where + "position columns must be 4-byte floats");
            }
        }
        if (!(p.step > 0.0f) || !std::isfinite(p.step)) {
            return Fail(error, where + "position step must be a positive, finite number");
        }
        if (!(p.range > 0.0f) || !std::isfinite(p.range)) {
            return Fail(error, where + "position range must be a positive, finite number");
        }
        const double steps = std::round(2.0 * static_cast<double>(p.range) / static_cast<double>(p.step));
        if (!(steps <= static_cast<double>(kMaxPositionOrdinal))) {
            return Fail(error, where + "position range divided by step is too large (limit 2^31 - 1 steps)");
        }
        p.words = 3;
        p.max_ordinal = static_cast<uint64_t>(steps);
        p.absolute_bits = BitsFor(p.max_ordinal);
        return true;
    }
    case PropertyKind::Angle:
        if (p.source[0].element_bytes != 4) {
            return Fail(error, where + "angle column must be a 4-byte float");
        }
        if (p.bits < 1 || p.bits > 16) {
            return Fail(error, where + "angle bits must be 1 to 16");
        }
        p.words = 1;
        p.max_ordinal = (1ull << p.bits) - 1;
        p.absolute_bits = p.bits;
        return true;
    case PropertyKind::Integer: {
        const uint8_t bytes = p.source[0].element_bytes;
        if (!IsIntegerElement(bytes)) {
            return Fail(error, where + "integer column element must be 1, 2, 4 or 8 bytes");
        }
        if (p.min_value > p.max_value) {
            return Fail(error, where + "integer minimum is above its maximum");
        }
        if (bytes < 8) {
            const uint32_t width = bytes * 8u;
            const int64_t low = p.is_signed ? -(int64_t{1} << (width - 1)) : 0;
            const int64_t high = p.is_signed ? (int64_t{1} << (width - 1)) - 1 : (int64_t{1} << width) - 1;
            if (p.min_value < low || p.max_value > high) {
                return Fail(error, where + "integer range does not fit its column");
            }
        } else if (!p.is_signed && p.min_value < 0) {
            return Fail(error, where + "unsigned 64-bit integer cannot have a negative minimum");
        }
        p.words = 2;
        p.max_ordinal = static_cast<uint64_t>(p.max_value) - static_cast<uint64_t>(p.min_value);
        p.absolute_bits = BitsFor(p.max_ordinal);
        return true;
    }
    case PropertyKind::Bits: {
        const uint8_t bytes = p.source[0].element_bytes;
        if (!IsIntegerElement(bytes)) {
            return Fail(error, where + "bit mask column element must be 1, 2, 4 or 8 bytes");
        }
        if (p.bits < 1 || p.bits > 64) {
            return Fail(error, where + "bit mask width must be 1 to 64");
        }
        if (p.bits > bytes * 8u) {
            return Fail(error, where + "bit mask is wider than its column");
        }
        p.words = 2;
        p.max_ordinal = p.bits == 64 ? ~0ull : (1ull << p.bits) - 1;
        p.absolute_bits = p.bits;
        return true;
    }
    case PropertyKind::Enum:
        if (!IsIntegerElement(p.source[0].element_bytes)) {
            return Fail(error, where + "enum column element must be 1, 2, 4 or 8 bytes");
        }
        if (p.enum_count < 2) {
            return Fail(error, where + "enum needs at least 2 values");
        }
        p.words = 1;
        p.max_ordinal = p.enum_count - 1ull;
        p.absolute_bits = BitsFor(p.max_ordinal);
        return true;
    }
    return Fail(error, where + "unknown property kind");
}

// FNV-1a over fixed-size fields, so the fingerprint does not depend on the platform.
class Fnv {
public:
    void Add(uint64_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            hash_ ^= (value >> (8 * i)) & 0xFF;
            hash_ *= 0x100000001B3ull;
        }
    }
    uint64_t value() const {
        return hash_;
    }

private:
    uint64_t hash_ = 0xCBF29CE484222325ull;
};

} // namespace

PropertyDescriptor PropertyDescriptor::Position(ColumnRef x, ColumnRef y, ColumnRef z, float step,
                                                float range) {
    PropertyDescriptor p;
    p.kind = PropertyKind::Position;
    p.source[0] = x;
    p.source[1] = y;
    p.source[2] = z;
    p.step = step;
    p.range = range;
    return p;
}

PropertyDescriptor PropertyDescriptor::Angle(ColumnRef source, uint32_t bits) {
    PropertyDescriptor p;
    p.kind = PropertyKind::Angle;
    p.source[0] = source;
    p.bits = bits;
    return p;
}

PropertyDescriptor PropertyDescriptor::Integer(ColumnRef source, bool is_signed, int64_t min_value,
                                               int64_t max_value) {
    PropertyDescriptor p;
    p.kind = PropertyKind::Integer;
    p.source[0] = source;
    p.is_signed = is_signed;
    p.min_value = min_value;
    p.max_value = max_value;
    return p;
}

PropertyDescriptor PropertyDescriptor::Bits(ColumnRef source, uint32_t width) {
    PropertyDescriptor p;
    p.kind = PropertyKind::Bits;
    p.source[0] = source;
    p.bits = width;
    return p;
}

PropertyDescriptor PropertyDescriptor::Enum(ColumnRef source, uint32_t value_count) {
    PropertyDescriptor p;
    p.kind = PropertyKind::Enum;
    p.source[0] = source;
    p.enum_count = value_count;
    return p;
}

bool ReplicationSchema::AddType(uint32_t type, std::vector<PropertyDescriptor> properties,
                                std::string* error) {
    if (type > EntityId::kMaxType) {
        return Fail(error, "type " + std::to_string(type) + " is above the maximum entity type");
    }
    if (types_[type]) {
        return Fail(error, "type " + std::to_string(type) + " already has a schema");
    }
    if (properties.empty()) {
        return Fail(error, "a type needs at least one property");
    }
    if (properties.size() > kMaxPropertiesPerType) {
        return Fail(error, "a type can have at most 64 properties");
    }
    uint32_t words = 0;
    for (size_t i = 0; i < properties.size(); ++i) {
        PropertyDescriptor& property = properties[i];
        if (!Prepare(property, i, error)) {
            return false;
        }
        property.word_offset = words;
        words += property.words;
    }
    if (words > kMaxWordsPerEntity) {
        return Fail(error, "quantized state takes " + std::to_string(words) + " words, the limit is 32");
    }
    auto schema = std::make_unique<TypeSchema>();
    schema->type = type;
    schema->properties = std::move(properties);
    schema->words = words;
    types_[type] = std::move(schema);
    if (words > max_words_) {
        max_words_ = words;
    }
    return true;
}

uint64_t ReplicationSchema::Hash() const noexcept {
    Fnv fnv;
    fnv.Add(kFormatVersion, 4);
    for (const std::unique_ptr<TypeSchema>& entry : types_) {
        if (!entry) {
            continue;
        }
        fnv.Add(entry->type, 4);
        fnv.Add(entry->properties.size(), 4);
        for (const PropertyDescriptor& p : entry->properties) {
            fnv.Add(static_cast<uint64_t>(p.kind), 1);
            fnv.Add(std::bit_cast<uint32_t>(p.step), 4);
            fnv.Add(std::bit_cast<uint32_t>(p.range), 4);
            fnv.Add(p.bits, 4);
            fnv.Add(p.is_signed ? 1 : 0, 1);
            fnv.Add(static_cast<uint64_t>(p.min_value), 8);
            fnv.Add(static_cast<uint64_t>(p.max_value), 8);
            fnv.Add(p.enum_count, 4);
        }
    }
    return fnv.value();
}

} // namespace tessera::net
