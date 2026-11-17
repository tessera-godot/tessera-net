// SPDX-License-Identifier: Apache-2.0
// The C++ type of the column that holds each property. One table, used by the check (which gives the replication
// code columns of these sizes) and by the generator (which writes columns of these types), so that what was
// checked is what is generated.
#pragma once

#include "tessera/schema/schema_file.hpp"

#include <cstdint>
#include <string>

namespace tessera::schema {

struct ColumnSpec {
    std::string cpp_type; ///< `float`, `uint16_t`, ...
    uint8_t bytes = 0;    ///< The size of one element.
};

/// The type of one column of the property. A position has three columns of this type (x, y and z).
ColumnSpec ColumnFor(const PropertyDecl& property);

} // namespace tessera::schema
