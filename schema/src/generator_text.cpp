// SPDX-License-Identifier: Apache-2.0
#include "tessera/schema/generator_text.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
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
    // The digits come from std::to_chars in its scientific form: the shortest digits that read back as the
    // same float, the same ones in every standard library. (The plain form would not do: for a whole number
    // some libraries write every digit of its exact value, and others write the short digits and then zeros.)
    // The layout is made here, so that the text is the same on every system, and it does not look at the
    // locale.
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::scientific);
    std::string scientific(buffer, result.ptr);
    if (scientific.find_first_of("in") != std::string::npos) { // inf and nan
        return scientific;
    }
    const bool negative = scientific.front() == '-';
    const size_t e = scientific.find('e');
    std::string digits;
    for (size_t i = negative ? 1 : 0; i < e; ++i) {
        if (scientific[i] != '.') {
            digits += scientific[i];
        }
    }
    const int exponent = std::atoi(scientific.c_str() + e + 1);
    // Close to one, the number is written out; very big and very small numbers keep the exponent.
    if (exponent < -4 || exponent >= 15) {
        return scientific;
    }
    std::string text;
    if (exponent < 0) {
        text = "0." + std::string(static_cast<size_t>(-exponent - 1), '0') + digits;
    } else if (digits.size() <= static_cast<size_t>(exponent) + 1) {
        text = digits + std::string(static_cast<size_t>(exponent) + 1 - digits.size(), '0') + ".0";
    } else {
        text = digits.substr(0, static_cast<size_t>(exponent) + 1) + "." +
               digits.substr(static_cast<size_t>(exponent) + 1);
    }
    return negative ? "-" + text : text;
}

} // namespace tessera::schema
