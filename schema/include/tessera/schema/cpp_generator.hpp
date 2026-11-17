// SPDX-License-Identifier: Apache-2.0
// Writes the C++ header of a schema: the column structs, the constants, the functions that register the types and the
// ones that make a table for each type.
#pragma once

#include "tessera/schema/schema_file.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace tessera::schema {

struct CppOptions {
    std::string ns = "game_schema"; ///< The namespace of everything generated, `a::b` for a nested one.
    std::string source_name;        ///< The name of the schema file, written in the first line (no folders).
};

/// True when `name` can be the namespace of the generated code. Otherwise says why in `why` (when given).
bool IsValidNamespace(std::string_view name, std::string* why = nullptr);

/// How a float is written in C++ source: with as few digits as bring the same float back, always with a point or
/// an exponent, and the suffix `f`. The text does not depend on the locale.
std::string FormatFloat(float value);

/// The text of the header. The caller has checked the file (and gives its `hash`, from the check) and the namespace;
/// the types come out in the order of their number, and the same file always gives the same bytes.
std::string GenerateCpp(const SchemaFile& file, uint64_t hash, const CppOptions& options);

} // namespace tessera::schema
