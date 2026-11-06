// SPDX-License-Identifier: Apache-2.0
// Shared pieces for the replication tests: a table layout, a schema that uses every property kind, and ids.
#pragma once

#include "tessera/net/entity_id.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/replication/property_schema.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace replication_test {

using namespace tessera::net;

/// The columns of the test entity table, one array per column as the entity table expects.
struct TestColumns {
    float x[kPageSlots];
    float y[kPageSlots];
    float z[kPageSlots];
    float yaw[kPageSlots];
    int32_t health[kPageSlots];
    uint16_t flags[kPageSlots];
    uint8_t state[kPageSlots];
    int64_t score[kPageSlots];
};

inline constexpr uint32_t kMobType = 0;     // position, angle, integer, bit mask and enum
inline constexpr uint32_t kScoreType = 1;   // one 64-bit integer using its whole range
inline constexpr uint32_t kUnknownType = 9; // never given a schema

inline ReplicationSchema MakeTestSchema() {
    ReplicationSchema schema;
    schema.AddType(
        kMobType,
        {PropertyDescriptor::Position(TESSERA_COLUMN(TestColumns, x), TESSERA_COLUMN(TestColumns, y),
                                      TESSERA_COLUMN(TestColumns, z), 0.01f, 10000.0f),
         PropertyDescriptor::Angle(TESSERA_COLUMN(TestColumns, yaw), 10),
         PropertyDescriptor::Integer(TESSERA_COLUMN(TestColumns, health), true, -1000, 1000),
         PropertyDescriptor::Bits(TESSERA_COLUMN(TestColumns, flags), 16),
         PropertyDescriptor::Enum(TESSERA_COLUMN(TestColumns, state), 8)});
    schema.AddType(kScoreType, {PropertyDescriptor::Integer(TESSERA_COLUMN(TestColumns, score), true,
                                                            std::numeric_limits<int64_t>::min(),
                                                            std::numeric_limits<int64_t>::max())});
    return schema;
}

inline EntityId MakeEntity(uint32_t type, uint32_t index) {
    return EntityId::Make(0, type, index, 0);
}

} // namespace replication_test
