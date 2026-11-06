// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>

namespace tessera::net {

/// How an entity type decides who can see it.
enum class RelevancyMode : uint8_t {
    Spatial,        ///< Visible to connections with an observer within the type's range.
    AlwaysRelevant, ///< Visible to every connection.
    OwnerOnly,      ///< Visible only to the connection that owns it.
};

/// Visibility settings of one entity type (the type is `EntityId::type()`, 0 to 255).
struct TypeRelevancy {
    RelevancyMode mode = RelevancyMode::Spatial;
    /// Spatial mode: how far an observer can see this type, in world units. 0 means never visible.
    float range = 0.0f;
    /// Spatial mode: extra distance in which an entity that is already visible stays visible. It stops
    /// entities near the edge of the range from flickering in and out.
    float hysteresis_margin = 0.0f;
};

struct RelevancyConfig {
    uint32_t max_connections = 128;
    /// Each connection recomputes what it sees once every this many ticks (staggered between
    /// connections). Higher is cheaper but delays the moment an entity appears.
    uint32_t recalc_interval_ticks = 3;
    uint32_t max_observers_per_connection = 4;
    /// Most entities one connection can see at once. Also the size of its compact id space.
    uint32_t max_visible_per_connection = 65536;
    std::array<TypeRelevancy, 256> types{};
};

/// Throws std::invalid_argument, naming the field, if the configuration cannot be used.
void ValidateRelevancyConfig(const RelevancyConfig& config);

} // namespace tessera::net
