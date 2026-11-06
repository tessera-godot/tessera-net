// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tessera::net {

/// The kinds of replicated property. Values 5 to 255 are reserved for kinds still to be added (vectors,
/// quaternions, strings, arrays and so on): adding one later only adds a codec and a value here.
enum class PropertyKind : uint8_t {
    Position = 0, ///< Three floats, sent as small steps of a fixed size.
    Angle = 1,    ///< A float in radians, sent as a whole number of equal steps around the circle.
    Integer = 2,  ///< A whole number in a declared range.
    Bits = 3,     ///< Up to 64 flags kept in one integer.
    Enum = 4,     ///< One of a fixed number of values.
};

/// Where one column of a table lives inside the table's page columns struct.
struct ColumnRef {
    uint32_t offset = 0;       ///< Byte offset of the column's first element.
    uint8_t element_bytes = 0; ///< Size of one element: 4 for a float, 1, 2, 4 or 8 for an integer.
};

/// Names a column of `Columns` (a struct of arrays) for a `PropertyDescriptor`.
#define TESSERA_COLUMN(Columns, member)                                                                      \
    ::tessera::net::ColumnRef {                                                                              \
        static_cast<uint32_t>(offsetof(Columns, member)),                                                    \
            static_cast<uint8_t>(sizeof(std::declval<Columns&>().member[0]))                                 \
    }

/// One replicated property of an entity type: where to read it and how to quantize it. Build one with the
/// factory functions; `ReplicationSchema::AddType` checks it and fills in the last group of fields.
struct PropertyDescriptor {
    PropertyKind kind = PropertyKind::Position;
    ColumnRef source[3];     ///< Position: the x, y and z float columns. Other kinds: `source[0]`.
    float step = 0.0f;       ///< Position: world units per step.
    float range = 0.0f;      ///< Position: values are limited to -range..range.
    uint32_t bits = 0;       ///< Angle: bits per angle (1 to 16). Bits: number of flags (1 to 64).
    bool is_signed = false;  ///< Integer: whether the column holds a signed number.
    int64_t min_value = 0;   ///< Integer: smallest value, inclusive.
    int64_t max_value = 0;   ///< Integer: largest value, inclusive.
    uint32_t enum_count = 0; ///< Enum: number of values (at least 2).

    // Filled in by `ReplicationSchema::AddType`.
    uint32_t word_offset = 0;   ///< First of this property's 32-bit words in an entity's quantized state.
    uint32_t words = 0;         ///< How many 32-bit words it takes.
    uint32_t absolute_bits = 0; ///< Bits of one value written in full (per axis for a position).
    uint64_t max_ordinal = 0;   ///< Largest quantized value (per axis for a position).

    static PropertyDescriptor Position(ColumnRef x, ColumnRef y, ColumnRef z, float step, float range);
    /// `source` is a float column holding radians.
    static PropertyDescriptor Angle(ColumnRef source, uint32_t bits);
    static PropertyDescriptor Integer(ColumnRef source, bool is_signed, int64_t min_value, int64_t max_value);
    static PropertyDescriptor Bits(ColumnRef source, uint32_t width);
    static PropertyDescriptor Enum(ColumnRef source, uint32_t value_count);
};

/// The replicated properties of one entity type, in the order they are written.
struct TypeSchema {
    uint32_t type = 0;
    std::vector<PropertyDescriptor> properties;
    uint32_t words = 0; ///< Total 32-bit words of quantized state per entity.
};

/// The replicated properties of every entity type. Build it once at start-up, before creating anything
/// that uses it, and keep it alive (and in place) while the replication classes that were given it exist.
class ReplicationSchema {
public:
    static constexpr uint32_t kFormatVersion = 1;
    /// Quantized state of one entity may take at most this many 32-bit words (128 bytes).
    static constexpr uint32_t kMaxWordsPerEntity = 32;
    static constexpr uint32_t kMaxPropertiesPerType = 64;

    /// Adds the schema of one entity type. Returns false, changing nothing and describing the problem in
    /// `error` when given, if the type is out of range or already has a schema, or a property is invalid.
    bool AddType(uint32_t type, std::vector<PropertyDescriptor> properties, std::string* error = nullptr);

    /// The schema of a type, or nullptr when it has none (it is then not replicated).
    const TypeSchema* Find(uint32_t type) const noexcept {
        return type < types_.size() ? types_[type].get() : nullptr;
    }

    /// The largest `words` among the types.
    uint32_t MaxWords() const noexcept {
        return max_words_;
    }

    /// A fingerprint of everything that decides what the bytes mean (format version, types and the
    /// parameters of each property), so a client can tell it was built from a different schema. Where the
    /// server reads a value from is local layout and is left out.
    uint64_t Hash() const noexcept;

private:
    std::array<std::unique_ptr<TypeSchema>, 256> types_;
    uint32_t max_words_ = 0;
};

} // namespace tessera::net
