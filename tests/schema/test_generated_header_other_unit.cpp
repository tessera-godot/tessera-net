// SPDX-License-Identifier: Apache-2.0
// A second translation unit that includes the generated header. If a function in it were not `inline`, the
// two units would define it twice and the test program would not link.
#include "all_kinds.hpp"

#include <cstdint>

bool OtherUnitAddsTheTypes(tessera::net::ReplicationSchema& schema) {
    return all_kinds::AddGeneratedTypes(schema);
}

uint64_t OtherUnitHash() {
    return all_kinds::kSchemaHash;
}
