// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "tessera/net/entity_id.hpp"

namespace tessera::net {

/// Maps entity type numbers (0 to 255) to readable names.
///
/// Filled once at start-up and then only read. Names are used for diagnostics and, later, to match
/// the types declared in the schema.
class TypeRegistry {
public:
    /// Registers a type. Fails if the number is out of range or if the number or the name is taken.
    bool Register(uint32_t type, std::string name) {
        if (type > EntityId::kMaxType || name.empty() || !names_[type].empty()) {
            return false;
        }
        for (const std::string& existing : names_) {
            if (existing == name) {
                return false;
            }
        }
        names_[type] = std::move(name);
        ++count_;
        return true;
    }

    /// Name of a registered type, or nullptr when the type is unknown.
    const std::string* NameOf(uint32_t type) const noexcept {
        if (type > EntityId::kMaxType || names_[type].empty()) {
            return nullptr;
        }
        return &names_[type];
    }

    size_t Count() const noexcept { return count_; }

private:
    std::array<std::string, EntityId::kMaxType + 1> names_;
    size_t count_ = 0;
};

}  // namespace tessera::net
