// SPDX-License-Identifier: Apache-2.0
// Writes the GDScript of a schema: a script with the type constants, the hash, and a function that builds the
// TesseraSchema that describes the same types as the schema file does.
#pragma once

#include "tessera/schema/schema_file.hpp"

#include <cstdint>
#include <string>

namespace tessera::schema {

struct GdScriptOptions {
    std::string class_name = "GameSchema"; ///< The `class_name` of the script.
    std::string source_name;               ///< The name of the schema file, written in the first line.
};

/// The text of the script. The caller has checked the file (and gives its `hash`, from the check) and the
/// class name; the types come out in the order of their number, and the same file always gives the same
/// bytes.
std::string GenerateGdScript(const SchemaFile& file, uint64_t hash, const GdScriptOptions& options);

} // namespace tessera::schema
