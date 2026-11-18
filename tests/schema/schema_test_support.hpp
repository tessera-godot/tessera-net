// SPDX-License-Identifier: Apache-2.0
// Small helpers for the tests of the schema tool.
#pragma once

#include "tessera/schema/schema_file.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace schema_test {

/// Where the first (or `nth`) `needle` starts in `text`, as a line and a column.
inline tessera::schema::json::Position At(const std::string& text, const std::string& needle,
                                          size_t nth = 0) {
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

/// A whole schema file with one type, whose properties are the text between the brackets (one per line, from
/// line 2).
inline std::string OneType(const std::string& properties,
                           const std::string& type_fields = "\"type\": 1, \"name\": \"entity\"") {
    return "{\"format_version\": 1, \"types\": [{" + type_fields + ", \"properties\": [\n" + properties +
           "\n]}]}";
}

/// The bytes of a file of tests/schema (a fixture or a golden file).
inline std::string ReadTestFile(const std::string& relative_path) {
    std::ifstream file(std::string(TESSERA_SCHEMA_TEST_DIR) + "/" + relative_path, std::ios::binary);
    if (!file) {
        std::printf("  test bug: cannot open %s\n", relative_path.c_str());
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

/// A folder made for one test, in the temporary folder of the system, and removed (with what is in it) at the
/// end.
class TempDir {
public:
    TempDir() {
        std::random_device random;
        path_ = std::filesystem::temp_directory_path() /
                ("tessera_schema_test_" + std::to_string(random()) + "_" + std::to_string(random()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& Path() const {
        return path_;
    }
    /// The path of a file or folder inside, with forward slashes.
    std::string File(const std::string& name) const {
        return (path_ / name).generic_string();
    }
    /// Writes a file inside (in binary), making the folders it needs.
    std::string Write(const std::string& name, const std::string& bytes) const {
        const std::filesystem::path target = path_ / name;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return target.generic_string();
    }

private:
    std::filesystem::path path_;
};

/// The bytes of a file at a path (empty when there is none).
inline std::string ReadAll(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

/// True when `text` has something that looks like a date or a time of day (`2026-11-18`, `12:34:56`), or says
/// so
/// (`__DATE__`, `__TIME__`, `UTC`). The year alone is not looked for: a number such as 9223372036854775807
/// has four digits that are a year, and the test would fail by itself in the year they make.
inline bool HasDateOrTime(const std::string& text) {
    const auto digits = [&](size_t at, size_t count) {
        if (at + count > text.size()) {
            return false;
        }
        for (size_t i = at; i < at + count; ++i) {
            if (text[i] < '0' || text[i] > '9') {
                return false;
            }
        }
        return true;
    };
    for (size_t i = 0; i < text.size(); ++i) {
        const bool date = digits(i, 4) && i + 10 <= text.size() && text[i + 4] == '-' && digits(i + 5, 2) &&
                          text[i + 7] == '-' && digits(i + 8, 2);
        const bool time = digits(i, 2) && i + 8 <= text.size() && text[i + 2] == ':' && digits(i + 3, 2) &&
                          text[i + 5] == ':' && digits(i + 6, 2);
        if (date || time) {
            return true;
        }
    }
    return text.find("__DATE__") != std::string::npos || text.find("__TIME__") != std::string::npos ||
           text.find("UTC") != std::string::npos;
}

inline bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

/// Checks that diagnostic `index` is at `where`, has `message_part` in its message and `fix_part` in its
/// correction (an empty `fix_part` is not looked at, and "-" means that there must be no correction).
inline void ExpectDiagnostic(const std::vector<tessera::schema::Diagnostic>& all, size_t index,
                             tessera::schema::json::Position where, const std::string& message_part,
                             const std::string& fix_part) {
    TESSERA_CHECK(index < all.size());
    if (index >= all.size()) {
        return;
    }
    const tessera::schema::Diagnostic& d = all[index];
    const bool ok = d.position.line == where.line && d.position.column == where.column &&
                    Has(d.message, message_part) &&
                    (fix_part.empty() || (fix_part == "-" ? d.fix.empty() : Has(d.fix, fix_part)));
    TESSERA_CHECK(ok);
    if (!ok) {
        std::printf("    wanted %u:%u \"%s\" fix \"%s\"\n    got    %u:%u \"%s\" fix \"%s\"\n", where.line,
                    where.column, message_part.c_str(), fix_part.c_str(), d.position.line, d.position.column,
                    d.message.c_str(), d.fix.c_str());
    }
}

} // namespace schema_test
