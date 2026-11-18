// SPDX-License-Identifier: Apache-2.0
#include "tessera/schema/generator_text.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <limits>

namespace tessera::schema {

std::string Hex16(uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "0x%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::string Int64Literal(int64_t value) {
    if (value == std::numeric_limits<int64_t>::min()) {
        return "(-9223372036854775807 - 1)"; // the literal -9223372036854775808 does not compile in C++
    }
    return std::to_string(value);
}

std::string NoticeName(const std::string& name) {
    const auto plain = [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte >= 0x20 && byte <= 0x7E && c != '\\';
    };
    if (name.empty() || !std::all_of(name.begin(), name.end(), plain)) {
        return "the schema file";
    }
    return name;
}

std::vector<const TypeDecl*> TypesInOrder(const SchemaFile& file) {
    std::vector<const TypeDecl*> types;
    for (const TypeDecl& type : file.types) {
        types.push_back(&type);
    }
    std::stable_sort(types.begin(), types.end(),
                     [](const TypeDecl* a, const TypeDecl* b) { return a->type < b->type; });
    return types;
}

std::string FloatText(float value) {
    // std::to_chars gives the shortest text that reads back as the same float, and it never looks at the
    // locale.
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    std::string text(buffer, result.ptr);
    if (text.find_first_of(".eEn") == std::string::npos) { // no point, exponent, or inf and nan
        text += ".0";
    }
    return text;
}

} // namespace tessera::schema
