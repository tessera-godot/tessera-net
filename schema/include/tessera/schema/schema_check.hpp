// SPDX-License-Identifier: Apache-2.0
// Checks a schema file by building the replication schema the server would build, with the replication code
// itself. What the server refuses is refused here, with the same words, and the hash is the one the server
// computes: nothing of the rules or of the fingerprint is written a second time.
#pragma once

#include "tessera/net/replication/property_schema.hpp"
#include "tessera/schema/schema_file.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace tessera::schema {

struct CheckResult {
    std::unique_ptr<net::ReplicationSchema> schema; ///< Null when anything is wrong.
    std::vector<Diagnostic> diagnostics;
    uint64_t hash = 0; ///< `schema->Hash()`, 0 when there is no schema.
};

/// Builds the schema of the file, the types in order of their number.
CheckResult Check(const SchemaFile& file);

/// The descriptor of one property. Its columns are placeholders of the right size: the hash does not depend
/// on where a column is.
net::PropertyDescriptor MakeDescriptor(const PropertyDecl& property);

} // namespace tessera::schema
