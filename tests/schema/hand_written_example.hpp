// SPDX-License-Identifier: Apache-2.0
// The schema of the example game as it was written by hand, before it came from a schema file. It is kept here, as
// a copy of the one in tests/server/test_server_example_schema.cpp, to compare what the tool builds with it.
#pragma once

#include "tessera/net/entity_table.hpp"
#include "tessera/net/replication/property_schema.hpp"

#include <cstddef>
#include <cstdint>

namespace schema_test {

struct HandWrittenEntityColumns {
    float x[tessera::net::kPageSlots];
    float y[tessera::net::kPageSlots];
    float z[tessera::net::kPageSlots];
};

struct HandWrittenPlayerColumns {
    float x[tessera::net::kPageSlots];
    float y[tessera::net::kPageSlots];
    float z[tessera::net::kPageSlots];
    uint16_t owner[tessera::net::kPageSlots];
};

inline tessera::net::ReplicationSchema HandWrittenSchema(bool players) {
    using tessera::net::PropertyDescriptor;
    tessera::net::ReplicationSchema schema;
    schema.AddType(1, {PropertyDescriptor::Position(
                          TESSERA_COLUMN(HandWrittenEntityColumns, x), TESSERA_COLUMN(HandWrittenEntityColumns, y),
                          TESSERA_COLUMN(HandWrittenEntityColumns, z), 0.01f, 2048.0f)});
    if (players) {
        schema.AddType(
            2, {PropertyDescriptor::Position(
                    TESSERA_COLUMN(HandWrittenPlayerColumns, x), TESSERA_COLUMN(HandWrittenPlayerColumns, y),
                    TESSERA_COLUMN(HandWrittenPlayerColumns, z), 0.01f, 2048.0f),
                PropertyDescriptor::Integer(TESSERA_COLUMN(HandWrittenPlayerColumns, owner), false, 0, 65535)});
    }
    return schema;
}

/// The hashes of that schema: without the players, and with them.
inline constexpr uint64_t kHandWrittenHashWithoutPlayers = 1615724867110258213ull;
inline constexpr uint64_t kHandWrittenHashWithPlayers = 339718250426401488ull;

} // namespace schema_test
