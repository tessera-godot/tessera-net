// SPDX-License-Identifier: Apache-2.0
// The schema file: what is in it and how it is read. Reading only checks what the file itself can get wrong
// (the JSON, the fields, the kinds of the values, the names, the repeated numbers). Whether the properties
// make a schema the replication code accepts is decided by `Check`, with the replication code itself.
#pragma once

#include "tessera/schema/json_reader.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::schema {

/// The kinds of property the file can describe, with the names `TesseraSchema.add_type` uses in GDScript.
enum class Kind { Position, Angle, Integer, Flag, Mask, Choice };

/// Where something is in the file.
struct Source {
    json::Position position;
};

struct PropertyDecl {
    std::string name;
    Kind kind = Kind::Flag;
    double step = 0;        ///< Position
    double range = 0;       ///< Position
    uint32_t bits = 0;      ///< Angle and mask
    uint32_t count = 0;     ///< Choice
    bool is_signed = false; ///< Integer
    int64_t min_value = 0;  ///< Integer
    int64_t max_value = 0;  ///< Integer
    std::string note;
    Source where;      ///< The object of the property.
    Source name_where; ///< The value of its "name".
};

struct TypeDecl {
    uint32_t type = 0;
    std::string name;
    std::string note;
    std::vector<PropertyDecl> properties;
    Source where;      ///< The object of the type.
    Source name_where; ///< The value of its "name".
};

struct SchemaFile {
    uint32_t format_version = 0;
    std::vector<TypeDecl> types;
};

/// One problem, with where it is and, when there is an obvious way, how to correct it.
struct Diagnostic {
    json::Position position;
    std::string message;
    std::string fix;
};

struct ReadResult {
    std::optional<SchemaFile> file; ///< Set only when there is no diagnostic.
    std::vector<Diagnostic> diagnostics;
    size_t omitted = 0; ///< How many more problems there were than the `kMaxDiagnostics` kept.
};

/// How text from the file is shown in a message: cut to 40 characters (with `...`), and a byte that is not a printable ASCII
/// character written as `\xNN`, so that a message is short and cannot carry a control byte to the terminal.
std::string EchoText(std::string_view text);

/// The most diagnostics one reading keeps (the first ones in the file; the rest are counted in `omitted`).
inline constexpr size_t kMaxDiagnostics = 20;

/// Reads the text of a schema file. A JSON error ends the reading (after it, positions cannot be trusted);
/// problems of the schema itself are all collected, in the order they are in the file.
ReadResult ReadSchema(std::string_view text);

/// `name:line:column: error: message`, then, when there is a correction, a line `  fix: ...` (no line break
/// at the end).
std::string Format(std::string_view file_name, const Diagnostic& diagnostic);

/// The names of the six kinds, in the order of `Kind`.
std::string_view KindName(Kind kind);

} // namespace tessera::schema
