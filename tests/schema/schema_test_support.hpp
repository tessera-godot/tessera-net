// SPDX-License-Identifier: Apache-2.0
// Small helpers for the tests of the schema tool.
#pragma once

#include "tessera/schema/schema_file.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace schema_test {

/// Where the first (or `nth`) `needle` starts in `text`, as a line and a column.
inline tessera::schema::json::Position At(const std::string& text, const std::string& needle, size_t nth = 0) {
    size_t from = 0;
    size_t found = std::string::npos;
    for (size_t i = 0; i <= nth; ++i) {
        found = text.find(needle, from);
        if (found == std::string::npos) {
            std::printf("  test bug: %s not found in the text\n", needle.c_str());
            return {};
        }
        from = found + 1;
    }
    tessera::schema::json::Position position;
    for (size_t i = 0; i < found; ++i) {
        if (text[i] == '\n') {
            ++position.line;
            position.column = 1;
        } else {
            ++position.column;
        }
    }
    return position;
}

/// A whole schema file with one type, whose properties are the text between the brackets (one per line, from line 2).
inline std::string OneType(const std::string& properties, const std::string& type_fields = "\"type\": 1, \"name\": \"entity\"") {
    return "{\"format_version\": 1, \"types\": [{" + type_fields + ", \"properties\": [\n" + properties + "\n]}]}";
}

/// The bytes of a file of tests/schema (a fixture or a golden file).
inline std::string ReadTestFile(const std::string& relative_path) {
    std::ifstream file(std::string(TESSERA_SCHEMA_TEST_DIR) + "/" + relative_path, std::ios::binary);
    if (!file) {
        std::printf("  test bug: cannot open %s\n", relative_path.c_str());
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

inline bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

/// Checks that diagnostic `index` is at `where`, has `message_part` in its message and `fix_part` in its correction
/// (an empty `fix_part` is not looked at, and "-" means that there must be no correction).
inline void ExpectDiagnostic(const std::vector<tessera::schema::Diagnostic>& all, size_t index,
                             tessera::schema::json::Position where, const std::string& message_part,
                             const std::string& fix_part) {
    TESSERA_CHECK(index < all.size());
    if (index >= all.size()) {
        return;
    }
    const tessera::schema::Diagnostic& d = all[index];
    const bool ok = d.position.line == where.line && d.position.column == where.column &&
                    Has(d.message, message_part) && (fix_part.empty() || (fix_part == "-" ? d.fix.empty() : Has(d.fix, fix_part)));
    TESSERA_CHECK(ok);
    if (!ok) {
        std::printf("    wanted %u:%u \"%s\" fix \"%s\"\n    got    %u:%u \"%s\" fix \"%s\"\n", where.line, where.column,
                    message_part.c_str(), fix_part.c_str(), d.position.line, d.position.column, d.message.c_str(),
                    d.fix.c_str());
    }
}

} // namespace schema_test
