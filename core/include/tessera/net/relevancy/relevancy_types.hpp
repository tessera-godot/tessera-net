// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <vector>

#include "tessera/net/entity_id.hpp"

namespace tessera::net {

/// Identifies a client connection. Valid values are smaller than `RelevancyConfig::max_connections`.
using ConnectionId = uint32_t;

/// Returned when an entity has no compact id on a connection (it is not visible there).
inline constexpr uint32_t kInvalidCompactId = 0xFFFFFFFFu;

/// A point from which a connection sees the world, for example a player's camera or a security
/// camera. A connection may have several.
struct Observer {
    float x;
    float y;
    float z;
};

/// A change in what a connection can see.
struct VisibilityEvent {
    enum class Kind : uint8_t {
        Enter,    ///< The entity became visible. `compact_id` is the id to use on this connection.
        Leave,    ///< The entity is no longer visible but still exists.
        Destroy,  ///< The entity no longer exists.
    };

    Kind kind;
    EntityId id;
    uint32_t compact_id;
};

/// The events of one connection, in the order they happened. The caller clears them after use.
struct ConnectionEvents {
    std::vector<VisibilityEvent> events;
};

}  // namespace tessera::net
