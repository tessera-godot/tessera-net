// SPDX-License-Identifier: Apache-2.0
// Builds the core's replication schema from a plain description (names, kinds and numbers), the way a script
// gives it. Nothing here knows about Godot, so all of it is tested on its own.
#pragma once

#include "tessera/net/replication/property_schema.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace tessera::ext {

/// One property of a type, as described. `kind` is one of "position", "angle", "integer", "flag", "mask" and
/// "choice"; only the numbers that kind uses are read:
///   position: step, range      angle: bits       integer: min_value, max_value, is_signed
///   flag: none                 mask: bits        choice: count
struct PropertyDescription {
    std::string name;
    std::string kind;
    double step = 0.0;
    double range = 0.0;
    uint32_t bits = 0;
    bool is_signed = false;
    int64_t min_value = 0;
    int64_t max_value = 0;
    uint32_t count = 0;
};

struct TypeDescription {
    uint32_t type = 0;
    std::vector<PropertyDescription> properties;
};

/// What a property turns into when it is read back.
enum class ValueKind : uint8_t { Position, Angle, Integer, Flag, Mask, Choice };

struct PropertyInfo {
    std::string name;
    ValueKind kind = ValueKind::Integer;
};

/// A schema the core can use, with the names that go with its properties. Not copyable: a client holds a
/// reference to the core schema inside, which stays at the same address for as long as this object lives.
class BuiltSchema {
public:
    BuiltSchema(std::unique_ptr<net::ReplicationSchema> schema,
                std::map<uint32_t, std::vector<PropertyInfo>> properties);
    BuiltSchema(const BuiltSchema&) = delete;
    BuiltSchema& operator=(const BuiltSchema&) = delete;
    BuiltSchema(BuiltSchema&&) noexcept = default;
    BuiltSchema& operator=(BuiltSchema&&) noexcept = default;

    const net::ReplicationSchema& Schema() const noexcept {
        return *schema_;
    }

    /// The names and kinds of a type's properties, in order; null for a type the schema does not have.
    const std::vector<PropertyInfo>* Properties(uint32_t type) const noexcept;

    uint64_t Hash() const noexcept {
        return schema_->Hash();
    }

private:
    std::unique_ptr<net::ReplicationSchema> schema_;
    std::map<uint32_t, std::vector<PropertyInfo>> properties_;
};

/// `schema` is null exactly when `error` is not empty.
struct BuildResult {
    std::unique_ptr<BuiltSchema> schema;
    std::string error;
};

/// Builds the schema for a set of types. The order of the types does not matter, the order of each type's
/// properties does.
BuildResult BuildSchema(std::span<const TypeDescription> types);

/// Empty if one type is fine, otherwise the problem, naming the type and the property.
std::string ValidateType(const TypeDescription& type);

} // namespace tessera::ext
