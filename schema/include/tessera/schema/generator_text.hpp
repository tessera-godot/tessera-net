// SPDX-License-Identifier: Apache-2.0
// The small pieces of text that the C++ generator and the GDScript generator share, so that a number or a
// name is written the same way by both.
#pragma once

#include "tessera/schema/schema_file.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tessera::schema {

/// `0x` and 16 hexadecimal digits, the way `tessera_schema hash` prints a hash.
std::string Hex16(uint64_t value);

/// A signed 64-bit number as source text. The smallest one is written as `(-9223372036854775807 - 1)`,
/// because its own literal does not compile in C++ and is easy to get wrong in GDScript.
std::string Int64Literal(int64_t value);

/// The name of the schema file for the first line of a generated file. It goes into a comment, so it has to
/// be plain ASCII on one line, and without a backslash. Another name is replaced by `the schema file`.
std::string NoticeName(const std::string& name);

/// The types of the file in the order of their numbers (the file may list them in any order).
std::vector<const TypeDecl*> TypesInOrder(const SchemaFile& file);

/// A float as text, with the fewest digits that give the same float back (at most 9). An exponent from -4 to
/// 14 is written out, always with a point (`0.0001`, `2048.0`, `100000000000000.0`); any other one is kept,
/// as in `1e-05` and `1e+30`. No suffix. The text is the same in every standard library, and the locale is
/// never looked at.
std::string FloatText(float value);

/// Builds a text one line at a time. Every line ends with "\n".
class Writer {
public:
    void Line(const std::string& text = {}) {
        text_ += text;
        text_ += '\n';
    }
    std::string Take() {
        return std::move(text_);
    }

private:
    std::string text_;
};

} // namespace tessera::schema
