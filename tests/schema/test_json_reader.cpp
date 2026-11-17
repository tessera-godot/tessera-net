// SPDX-License-Identifier: Apache-2.0
#include "tessera/schema/json_reader.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>

using namespace tessera::schema::json;

namespace {

// Parses text that has to be valid, and gives the value (or a null value, after a failed check).
Value Read(const std::string& text) {
    const ParseResult result = Parse(text);
    TESSERA_CHECK(result.value.has_value());
    return result.value ? *result.value : Value();
}

} // namespace

TESSERA_TEST(json_parses_scalars) {
    TESSERA_CHECK(Read("null").GetType() == Type::Null);
    TESSERA_CHECK(Read("true").GetType() == Type::Bool && Read("true").AsBool());
    TESSERA_CHECK(Read("false").GetType() == Type::Bool && !Read("false").AsBool());
    TESSERA_CHECK(Read("0").AsInteger() == 0);
    TESSERA_CHECK(Read("-12").AsInteger() == -12);
    TESSERA_CHECK(Read("3.5").AsNumber() == 3.5);
    TESSERA_CHECK(Read("1e3").AsNumber() == 1000.0);
    TESSERA_CHECK(Read("\"a\"").AsString() == "a");
}

TESSERA_TEST(json_parses_nested_objects_and_arrays) {
    const Value root = Read(R"({"a": [1, 2, {"b": null}], "c": {"d": "e"}, "f": []})");
    TESSERA_CHECK(root.GetType() == Type::Object);
    TESSERA_CHECK(root.Members().size() == 3);
    const Value* a = root.Find("a");
    TESSERA_CHECK(a != nullptr && a->GetType() == Type::Array && a->Items().size() == 3);
    TESSERA_CHECK(a != nullptr && a->Items()[2].Find("b") != nullptr);
    const Value* c = root.Find("c");
    TESSERA_CHECK(c != nullptr && c->Find("d") != nullptr && c->Find("d")->AsString() == "e");
    TESSERA_CHECK(root.Find("f") != nullptr && root.Find("f")->Items().empty());
    TESSERA_CHECK(root.Find("missing") == nullptr);
}

TESSERA_TEST(json_keeps_member_order_and_positions) {
    const Value root = Read("{\n  \"first\": 1,\n    \"second\": [true,\n  \"x\"]\n}");
    TESSERA_CHECK(root.position.line == 1 && root.position.column == 1);
    TESSERA_CHECK(root.Members().size() == 2);
    TESSERA_CHECK(root.Members()[0].key == "first" && root.Members()[1].key == "second");
    TESSERA_CHECK(root.Members()[0].key_position.line == 2 && root.Members()[0].key_position.column == 3);
    TESSERA_CHECK(root.Members()[0].value.position.line == 2 && root.Members()[0].value.position.column == 12);
    TESSERA_CHECK(root.Members()[1].key_position.line == 3 && root.Members()[1].key_position.column == 5);
    const Value& list = root.Members()[1].value;
    TESSERA_CHECK(list.position.line == 3 && list.position.column == 15);
    TESSERA_CHECK(list.Items()[0].position.line == 3 && list.Items()[0].position.column == 16);
    TESSERA_CHECK(list.Items()[1].position.line == 4 && list.Items()[1].position.column == 3);
}

TESSERA_TEST(json_distinguishes_integers_from_other_numbers) {
    TESSERA_CHECK(Read("1").GetType() == Type::Integer);
    TESSERA_CHECK(Read("1.0").GetType() == Type::Number);
    TESSERA_CHECK(Read("1e0").GetType() == Type::Number);
    TESSERA_CHECK(Read("1E2").GetType() == Type::Number);
    TESSERA_CHECK(Read("1E2").AsNumber() == 100.0);
    TESSERA_CHECK(Read("7").AsNumber() == 7.0); // an integer can be read as a number
    TESSERA_CHECK(Read("7").IsNumeric() && Read("7.5").IsNumeric() && !Read("\"7\"").IsNumeric());
}

TESSERA_TEST(json_parses_string_escapes) {
    // The texts are kept out of the check macro: its failure message trips the preprocessor of some compilers
    // when it holds a raw string.
    const std::string simple = Read("\"\\\" \\\\ \\/ \\b \\f \\n \\r \\t \\u0041\"").AsString();
    TESSERA_CHECK(simple == "\" \\ / \b \f \n \r \t A");
    const std::string emoji = Read("\"\\ud83d\\ude00\"").AsString();
    TESSERA_CHECK(emoji == "\xF0\x9F\x98\x80");
    const std::string accent = Read("\"\\u00e9\"").AsString();
    TESSERA_CHECK(accent == "\xC3\xA9");
    const std::string euro = Read("\"\\u20ac\"").AsString();
    TESSERA_CHECK(euro == "\xE2\x82\xAC");
}

TESSERA_TEST(json_accepts_crlf_and_tabs_between_tokens) {
    const Value root = Read("{\r\n\t\"a\":\t1,\r\n\t\"b\" :\r\n2\r\n}\r\n");
    TESSERA_CHECK(root.Find("a") != nullptr && root.Find("a")->AsInteger() == 1);
    TESSERA_CHECK(root.Find("b") != nullptr && root.Find("b")->AsInteger() == 2);
}

TESSERA_TEST(json_skips_a_leading_byte_order_mark) {
    const Value root = Read("\xEF\xBB\xBF{\"a\": 1}");
    TESSERA_CHECK(root.Find("a") != nullptr);
    TESSERA_CHECK(root.position.line == 1 && root.position.column == 1);
}

TESSERA_TEST(json_parses_int64_limits) {
    TESSERA_CHECK(Read("9223372036854775807").AsInteger() == INT64_MAX);
    TESSERA_CHECK(Read("-9223372036854775808").AsInteger() == INT64_MIN);
    TESSERA_CHECK(Read("-0").AsInteger() == 0);
}

TESSERA_TEST(json_number_text_round_trips_doubles) {
    for (const char* text : {"0.01", "2048.0", "1e-7", "123456789.125", "0.1", "-4.25e2"}) {
        TESSERA_CHECK(Read(text).AsNumber() == std::strtod(text, nullptr));
    }
}
