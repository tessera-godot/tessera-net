// SPDX-License-Identifier: Apache-2.0
#include "tessera/schema/json_reader.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

using namespace tessera::schema::json;

namespace {

bool Has(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

// Checks that `text` is refused at this line and column, with a message that has `message_part`, and a correction that
// has `fix_part` (or no correction when it is null).
void ExpectError(const std::string& text, uint32_t line, uint32_t column, const char* message_part,
                 const char* fix_part) {
    const ParseResult result = Parse(text);
    TESSERA_CHECK(!result.value.has_value());
    TESSERA_CHECK(result.error.has_value());
    if (!result.error) {
        return;
    }
    TESSERA_CHECK(result.error->position.line == line);
    TESSERA_CHECK(result.error->position.column == column);
    TESSERA_CHECK(Has(result.error->message, message_part));
    if (fix_part == nullptr) {
        TESSERA_CHECK(result.error->fix.empty());
    } else {
        TESSERA_CHECK(Has(result.error->fix, fix_part));
    }
    if (result.error->position.line != line || result.error->position.column != column ||
        !Has(result.error->message, message_part)) {
        std::printf("    for %s: %u:%u %s\n", text.substr(0, 40).c_str(), result.error->position.line,
                    result.error->position.column, result.error->message.c_str());
    }
}

} // namespace

TESSERA_TEST(json_empty_input_is_an_error) {
    ExpectError("", 1, 1, "empty", nullptr);
    ExpectError("  \n ", 2, 2, "empty", nullptr);
    ExpectError("\xEF\xBB\xBF", 1, 1, "empty", nullptr);
}

TESSERA_TEST(json_trailing_comma_says_how_to_fix_it) {
    ExpectError("[1, 2,]", 1, 6, "comma after the last item", "remove the comma");
    ExpectError("{\"a\": 1,\n}", 1, 8, "comma after the last member", "remove the comma");
}

TESSERA_TEST(json_comment_says_to_use_note) {
    ExpectError("{\n  // the entity\n  \"a\": 1}", 2, 3, "comments are not allowed", "\"note\"");
    ExpectError("{\"a\": /* x */ 1}", 1, 7, "comments are not allowed", "\"note\"");
    ExpectError("{\"a\": 1} // after", 1, 10, "comments are not allowed", "\"note\"");
}

TESSERA_TEST(json_single_quotes_suggest_double_quotes) {
    ExpectError("{'a': 1}", 1, 2, "single quotes", "double quotes");
    ExpectError("{\"a\": 'b'}", 1, 7, "single quotes", "double quotes");
}

TESSERA_TEST(json_missing_comma_between_members) {
    ExpectError("{\"a\": 1\n \"b\": 2}", 2, 2, "\",\" or \"}\"", "add a comma");
    ExpectError("[1 2]", 1, 4, "\",\" or \"]\"", "add a comma");
    ExpectError("[1 ?]", 1, 4, "\",\" or \"]\"", nullptr);
}

TESSERA_TEST(json_unquoted_key) {
    ExpectError("{a: 1}", 1, 2, "key must be in double quotes", "\"a\"");
    ExpectError("{\"x\": 1, step: 2}", 1, 10, "key must be in double quotes", "\"step\"");
}

TESSERA_TEST(json_number_forms_json_does_not_allow) {
    ExpectError("01", 1, 1, "leading zero", "remove");
    ExpectError("[+1]", 1, 2, "plus sign", "remove");
    ExpectError("0x10", 1, 1, "hexadecimal", "decimal");
    ExpectError("[.5]", 1, 2, "decimal point", "0.5");
    ExpectError("1.", 1, 1, "decimal point must be followed by a digit", "add a digit");
    ExpectError("NaN", 1, 1, "NaN and Infinity", "finite");
    ExpectError("[Infinity]", 1, 2, "NaN and Infinity", "finite");
    ExpectError("-", 1, 1, "minus sign", "write the number");
    ExpectError("1e", 1, 1, "exponent", "digits");
}

TESSERA_TEST(json_integer_above_int64_is_an_error) {
    ExpectError("9223372036854775808", 1, 1, "does not fit in 64 bits", "smaller");
    ExpectError("-9223372036854775809", 1, 1, "does not fit in 64 bits", "smaller");
    ExpectError("[99999999999999999999]", 1, 2, "does not fit in 64 bits", "smaller");
}

TESSERA_TEST(json_double_out_of_range_is_an_error) {
    ExpectError("1e999", 1, 1, "too large", nullptr);
}

TESSERA_TEST(json_duplicate_key_names_both_places) {
    ExpectError("{\n  \"a\": 1,\n  \"b\": 2,\n  \"a\": 3\n}", 4, 3, "\"a\" appears twice", "remove one");
    const ParseResult result = Parse("{\n  \"a\": 1,\n  \"b\": 2,\n  \"a\": 3\n}");
    TESSERA_CHECK(result.error.has_value() && Has(result.error->message, "line 2"));
}

TESSERA_TEST(json_text_after_the_value_is_an_error) {
    ExpectError("{} x", 1, 4, "end of the file", nullptr);
    ExpectError("1 2", 1, 3, "end of the file", nullptr);
    ExpectError("[] []", 1, 4, "end of the file", nullptr);
}

TESSERA_TEST(json_bad_escapes_and_control_characters_in_strings) {
    ExpectError(R"("a\xb")", 1, 3, "not an escape", nullptr);
    ExpectError(R"("\u12")", 1, 2, "four hexadecimal digits", nullptr);
    ExpectError(R"("\ud83d")", 1, 2, "surrogate", nullptr);
    ExpectError(R"("\ud83dA")", 1, 2, "surrogate", nullptr);
    ExpectError(R"("\ude00")", 1, 2, "surrogate", nullptr);
    ExpectError(std::string("\"a\x01") + "b\"", 1, 3, "control character", nullptr);
}

TESSERA_TEST(json_unterminated_string_and_array_report_where_they_start) {
    ExpectError("{\"a\": \"abc", 1, 7, "string is never closed", "closing double quote");
    ExpectError("{\"a\": \"abc\n\"}", 1, 7, "string is never closed", "closing double quote");
    ExpectError("{\n \"a\": [1, 2", 2, 7, "array is never closed", "]");
    ExpectError("[\n {\"a\": 1,", 2, 2, "object is never closed", "}");
    ExpectError("[1,", 1, 1, "array is never closed", "]");
}

TESSERA_TEST(json_depth_limit) {
    const auto nested = [](int depth) { return std::string(static_cast<size_t>(depth), '[') + std::string(static_cast<size_t>(depth), ']'); };
    TESSERA_CHECK(Parse(nested(16)).value.has_value());
    const ParseResult too_deep = Parse(nested(17));
    TESSERA_CHECK(!too_deep.value.has_value() && too_deep.error.has_value());
    TESSERA_CHECK(too_deep.error.has_value() && Has(too_deep.error->message, "deeper than 16"));
    // A hostile file: it ends in an error, not in a stack overflow.
    const ParseResult hostile = Parse(std::string(100000, '['));
    TESSERA_CHECK(!hostile.value.has_value() && hostile.error.has_value());
    const ParseResult objects = Parse([] {
        std::string text;
        for (int i = 0; i < 20; ++i) {
            text += "{\"a\":";
        }
        return text;
    }());
    TESSERA_CHECK(objects.error.has_value() && Has(objects.error->message, "deeper than 16"));
}

TESSERA_TEST(json_size_limit) {
    Limits limits;
    limits.max_bytes = 20;
    const std::string fits = "[1," + std::string(15, ' ') + "2]";
    TESSERA_CHECK(fits.size() == 20);
    TESSERA_CHECK(Parse(fits, limits).value.has_value());
    const ParseResult over = Parse(fits + " ", limits);
    TESSERA_CHECK(!over.value.has_value() && over.error.has_value());
    TESSERA_CHECK(over.error.has_value() && Has(over.error->message, "larger than 20 bytes"));
    TESSERA_CHECK(over.error.has_value() && over.error->position.line == 1 && over.error->position.column == 1);
    // The default is one mebibyte.
    const std::string big = "[" + std::string((1u << 20) - 2, ' ') + "]";
    TESSERA_CHECK(Parse(big).value.has_value());
    TESSERA_CHECK(!Parse(big + " ").value.has_value());
    // And a string may be 1,024 bytes and no more.
    Limits strings;
    strings.max_string = 8;
    TESSERA_CHECK(Parse("\"12345678\"", strings).value.has_value());
    const ParseResult long_string = Parse("\"123456789\"", strings);
    TESSERA_CHECK(long_string.error.has_value() && Has(long_string.error->message, "longer than 8 bytes"));
    TESSERA_CHECK(Parse("\"" + std::string(1024, 'a') + "\"").value.has_value());
    TESSERA_CHECK(!Parse("\"" + std::string(1025, 'a') + "\"").value.has_value());
}

TESSERA_TEST(json_error_line_and_column_count_crlf_and_tabs) {
    // Line 1 ends with \r\n, line 2 starts with two tabs: the \r\n is one break and a tab is one column.
    ExpectError("{\r\n\t\t\"a\" 1}", 2, 7, "\":\" after the key", nullptr);
    ExpectError("[\r\n\r\n  ?]", 3, 3, "unexpected \"?\"", nullptr);
    // After a byte order mark the count starts again from 1.
    ExpectError("\xEF\xBB\xBF?", 1, 1, "unexpected \"?\"", nullptr);
}

TESSERA_TEST(json_never_overruns_on_truncated_input) {
    const std::string document =
        "{\"format_version\": 1, \"types\": [{\"type\": 1, \"name\": \"entity\", \"note\": \"A \\u00e9 \\ud83d\\ude00 note\", "
        "\"properties\": [{\"name\": \"position\", \"kind\": \"position\", \"step\": 0.01, \"range\": 2048.0}, "
        "{\"name\": \"owner\", \"kind\": \"integer\", \"min\": -5, \"max\": 65535, \"signed\": true, \"extra\": [null, true, false, "
        "1e-7, \"x\"]}]}, {\"type\": 2, \"name\": \"b\", \"properties\": []}]}";
    TESSERA_CHECK(Parse(document).value.has_value());
    for (size_t length = 0; length < document.size(); ++length) {
        // Each prefix is a copy of exactly that size, so that a read past the end is a read outside the allocation.
        const std::string prefix(document.data(), length);
        const ParseResult result = Parse(prefix);
        TESSERA_CHECK(!result.value.has_value() || result.error.has_value() == false);
        TESSERA_CHECK(result.value.has_value() != result.error.has_value());
    }
}
