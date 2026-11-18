// SPDX-License-Identifier: Apache-2.0
#include "schema_test_support.hpp"
#include "tessera/schema/schema_file.hpp"

#include <string>

using namespace tessera::schema;
using schema_test::At;
using schema_test::ExpectDiagnostic;
using schema_test::OneType;

namespace {

const char* kMinimal = R"({
  "format_version": 1,
  "types": [
    {"type": 1, "name": "entity", "properties": [
      {"name": "position", "kind": "position", "step": 0.01, "range": 2048.0}]}
  ]
})";

const std::string kPosition = R"({"name": "position", "kind": "position", "step": 0.01, "range": 2048.0})";

} // namespace

TESSERA_TEST(file_reads_the_minimal_valid_schema) {
    const ReadResult result = ReadSchema(kMinimal);
    TESSERA_CHECK(result.diagnostics.empty());
    TESSERA_CHECK(result.file.has_value());
    if (!result.file) {
        return;
    }
    TESSERA_CHECK(result.file->format_version == 1);
    TESSERA_CHECK(result.file->types.size() == 1);
    const TypeDecl& type = result.file->types[0];
    TESSERA_CHECK(type.type == 1 && type.name == "entity" && type.properties.size() == 1);
    TESSERA_CHECK(type.where.position.line == 4 && type.where.position.column == 5);
    const PropertyDecl& p = type.properties[0];
    TESSERA_CHECK(p.name == "position" && p.kind == Kind::Position && p.step == 0.01 && p.range == 2048.0);
    TESSERA_CHECK(p.where.position.line == 5 && p.where.position.column == 7);
}

TESSERA_TEST(file_reads_every_kind_with_its_numbers) {
    const std::string text = OneType(
        R"({"name": "pos", "kind": "position", "step": 1, "range": 100, "note": "Where."},
{"name": "yaw", "kind": "angle", "bits": 12},
{"name": "owner", "kind": "integer", "min": -5, "max": 70000, "signed": true},
{"name": "score", "kind": "integer", "min": 0, "max": 9},
{"name": "alive", "kind": "flag"},
{"name": "flags", "kind": "mask", "bits": 40},
{"name": "team", "kind": "choice", "count": 300})");
    const ReadResult result = ReadSchema(text);
    TESSERA_CHECK(result.diagnostics.empty());
    TESSERA_CHECK(result.file.has_value());
    if (!result.file) {
        return;
    }
    const auto& props = result.file->types[0].properties;
    TESSERA_CHECK(props.size() == 7);
    TESSERA_CHECK(props[0].kind == Kind::Position && props[0].step == 1.0 && props[0].range == 100.0 &&
                  props[0].note == "Where.");
    TESSERA_CHECK(props[1].kind == Kind::Angle && props[1].bits == 12);
    TESSERA_CHECK(props[2].kind == Kind::Integer && props[2].min_value == -5 && props[2].max_value == 70000 &&
                  props[2].is_signed);
    TESSERA_CHECK(props[3].kind == Kind::Integer && !props[3].is_signed); // "signed" is optional and false
    TESSERA_CHECK(props[4].kind == Kind::Flag);
    TESSERA_CHECK(props[5].kind == Kind::Mask && props[5].bits == 40);
    TESSERA_CHECK(props[6].kind == Kind::Choice && props[6].count == 300);
}

TESSERA_TEST(file_requires_format_version_1) {
    const std::string types =
        "\"types\": [{\"type\": 1, \"name\": \"e\", \"properties\": [" + kPosition + "]}]";
    const auto first = [&](const std::string& version_field) {
        return ReadSchema("{" + version_field + types + "}");
    };
    // Missing.
    ReadResult r = first("");
    TESSERA_CHECK(!r.file && r.diagnostics.size() == 1);
    ExpectDiagnostic(r.diagnostics, 0, {1, 1}, "needs a \"format_version\"", "\"format_version\": 1");
    // The number of another version.
    const std::string version2 = "\"format_version\": 2, ";
    r = first(version2);
    TESSERA_CHECK(!r.file && r.diagnostics.size() == 1);
    ExpectDiagnostic(r.diagnostics, 0, At("{" + version2, "2"), "reads format version 1, and the file says 2",
                     "set \"format_version\" to 1");
    // Text, and a decimal number.
    const std::string as_text = "\"format_version\": \"1\", ";
    r = first(as_text);
    ExpectDiagnostic(r.diagnostics, 0, At("{" + as_text, "\"1\""), "is a number, and here it is text",
                     "without quotes");
    const std::string as_decimal = "\"format_version\": 1.0, ";
    r = first(as_decimal);
    ExpectDiagnostic(r.diagnostics, 0, At("{" + as_decimal, "1.0"), "whole number, not a decimal one",
                     "write 1");
    const std::string as_list = "\"format_version\": [1], ";
    r = first(as_list);
    ExpectDiagnostic(r.diagnostics, 0, At("{" + as_list, "[1]"), "must be the whole number 1",
                     "\"format_version\": 1");
}

TESSERA_TEST(file_rejects_unknown_top_level_field) {
    const std::string text = "{\"format_version\": 1,\n \"typs\": [], \"extra\": 1}";
    const ReadResult r = ReadSchema(text);
    TESSERA_CHECK(!r.file);
    // "typs" is close to "types" and gets the hint; "extra" is not close to anything. The list is missing
    // too.
    ExpectDiagnostic(r.diagnostics, 0, {1, 1}, "needs a list of \"types\"", "");
    ExpectDiagnostic(r.diagnostics, 1, At(text, "\"typs\""), "unknown field \"typs\" in the file",
                     "did you mean \"types\"?");
    ExpectDiagnostic(r.diagnostics, 2, At(text, "\"extra\""), "unknown field \"extra\"", "-");
}

TESSERA_TEST(file_rejects_unknown_type_field) {
    const std::string text = "{\"format_version\": 1, \"types\": [{\"type\": 1, \"name\": \"e\", "
                             "\"propertis\": [], \"colour\": 3}]}";
    const ReadResult r = ReadSchema(text);
    TESSERA_CHECK(!r.file);
    ExpectDiagnostic(r.diagnostics, 0, At(text, "{\"type\""), "needs a list of \"properties\"", "");
    ExpectDiagnostic(r.diagnostics, 1, At(text, "\"propertis\""), "unknown field \"propertis\" in a type",
                     "did you mean \"properties\"?");
    ExpectDiagnostic(r.diagnostics, 2, At(text, "\"colour\""), "unknown field \"colour\" in a type", "-");
}

TESSERA_TEST(file_rejects_unknown_property_field_and_suggests_the_close_one) {
    const std::string text =
        OneType(R"({"name": "p", "kind": "position", "stp": 0.01, "range": 2, "colour": 1})");
    const ReadResult r = ReadSchema(text);
    TESSERA_CHECK(!r.file);
    ExpectDiagnostic(r.diagnostics, 0, At(text, "{\"name\": \"p\""), "needs the key \"step\"",
                     "add \"step\": a number");
    ExpectDiagnostic(r.diagnostics, 1, At(text, "\"stp\""), "unknown field \"stp\" in a property",
                     "did you mean \"step\"?");
    ExpectDiagnostic(r.diagnostics, 2, At(text, "\"colour\""), "unknown field \"colour\"", "-");
    // The hint only looks at the keys that this kind uses: "bits" is not one of a position's.
    const std::string other =
        OneType(R"({"name": "p", "kind": "position", "step": 1, "range": 2, "bitz": 3})");
    const ReadResult s = ReadSchema(other);
    ExpectDiagnostic(s.diagnostics, 0, At(other, "\"bitz\""), "unknown field \"bitz\"", "-");
}

TESSERA_TEST(file_rejects_a_key_that_belongs_to_another_kind) {
    const std::string text = OneType(R"({"name": "a", "kind": "flag", "bits": 3})");
    const ReadResult r = ReadSchema(text);
    TESSERA_CHECK(!r.file && r.diagnostics.size() == 1);
    ExpectDiagnostic(r.diagnostics, 0, At(text, "\"bits\""), "\"bits\" does not belong to the kind \"flag\"",
                     "takes no keys besides");
    const std::string text2 = OneType(R"({"name": "a", "kind": "mask", "bits": 3, "min": 0})");
    const ReadResult r2 = ReadSchema(text2);
    ExpectDiagnostic(r2.diagnostics, 0, At(text2, "\"min\""), "does not belong to the kind \"mask\"",
                     "uses \"bits\"");
}

TESSERA_TEST(file_rejects_a_missing_key_for_the_kind) {
    const std::string text = OneType(R"({"name": "a", "kind": "integer"})");
    const ReadResult r = ReadSchema(text);
    TESSERA_CHECK(!r.file && r.diagnostics.size() == 2);
    const auto where = At(text, "{\"name\": \"a\"");
    ExpectDiagnostic(r.diagnostics, 0, where, "needs the key \"min\"", "add \"min\": a whole number");
    ExpectDiagnostic(r.diagnostics, 1, where, "needs the key \"max\"", "add \"max\": a whole number");
    const std::string text2 = OneType(R"({"kind": "position", "step": 1})");
    const ReadResult r2 = ReadSchema(text2);
    TESSERA_CHECK(r2.diagnostics.size() == 2);
    ExpectDiagnostic(r2.diagnostics, 0, At(text2, "{\"kind\""), "a property needs a \"name\"",
                     "add \"name\"");
    ExpectDiagnostic(r2.diagnostics, 1, At(text2, "{\"kind\""), "needs the key \"range\"", "a number");
    const std::string text3 = OneType(R"({"name": "a"})");
    ExpectDiagnostic(ReadSchema(text3).diagnostics, 0, At(text3, "{\"name\""), "needs a \"kind\"",
                     "one of position, angle");
    const std::string text4 = OneType(R"({"name": "a", "kind": "positon"})");
    ExpectDiagnostic(ReadSchema(text4).diagnostics, 0, At(text4, "\"positon\""), "unknown kind \"positon\"",
                     "did you mean \"position\"?");
}

TESSERA_TEST(file_rejects_wrong_value_types) {
    const auto first = [](const std::string& property) { return ReadSchema(OneType(property)).diagnostics; };
    std::string text = OneType(R"({"name": "p", "kind": "position", "step": "0.01", "range": 2})");
    ExpectDiagnostic(first(R"({"name": "p", "kind": "position", "step": "0.01", "range": 2})"), 0,
                     At(text, "\"0.01\""), "\"step\" must be a number", "without quotes");
    text = OneType(R"({"name": "a", "kind": "angle", "bits": 12.0})");
    ExpectDiagnostic(first(R"({"name": "a", "kind": "angle", "bits": 12.0})"), 0, At(text, "12.0"),
                     "\"bits\" must be a whole number", "decimal point");
    text = OneType(R"({"name": "a", "kind": "integer", "min": 0, "max": 9, "signed": 1})");
    ExpectDiagnostic(first(R"({"name": "a", "kind": "integer", "min": 0, "max": 9, "signed": 1})"), 0,
                     At(text, "1}"), "\"signed\" must be true or false", "");
    text = OneType(R"({"name": "a", "kind": "choice", "count": -2})");
    ExpectDiagnostic(first(R"({"name": "a", "kind": "choice", "count": -2})"), 0, At(text, "-2"),
                     "\"count\" must be a whole number from 0 to 4294967295", "");
    text = OneType(R"({"name": 5, "kind": "flag"})");
    ExpectDiagnostic(first(R"({"name": 5, "kind": "flag"})"), 0, At(text, "5"),
                     "name of a property must be text", "");
    text = OneType(R"({"name": "a", "kind": 3})");
    ExpectDiagnostic(first(R"({"name": "a", "kind": 3})"), 0, At(text, "3"), "\"kind\" must be text", "");
    // Where a list or an object belongs.
    const std::string not_a_list = "{\"format_version\": 1, \"types\": {\"type\": 1}}";
    ExpectDiagnostic(ReadSchema(not_a_list).diagnostics, 0, At(not_a_list, "{\"type\""),
                     "\"types\" must be a list", "");
    const std::string not_an_object = "{\"format_version\": 1, \"types\": [3]}";
    ExpectDiagnostic(ReadSchema(not_an_object).diagnostics, 0, At(not_an_object, "3"),
                     "a type must be an object", "");
    const std::string properties_not_a_list =
        "{\"format_version\": 1, \"types\": [{\"type\": 1, \"name\": \"e\", \"properties\": {}}]}";
    ExpectDiagnostic(ReadSchema(properties_not_a_list).diagnostics, 0, At(properties_not_a_list, "{}"),
                     "\"properties\" must be a list", "");
    ExpectDiagnostic(ReadSchema("[]").diagnostics, 0, {1, 1}, "must hold one JSON object", "");
}

TESSERA_TEST(file_type_number_must_be_0_to_255) {
    for (const char* bad : {"-1", "256", "1.5", "\"1\"", "1000000"}) {
        const std::string fields = std::string("\"type\": ") + bad + ", \"name\": \"e\"";
        const std::string text = OneType(kPosition, fields);
        const ReadResult r = ReadSchema(text);
        TESSERA_CHECK(!r.file && r.diagnostics.size() == 1);
        ExpectDiagnostic(r.diagnostics, 0, At(text, bad), "type number", "from 0 to 255");
    }
    for (const char* good : {"0", "255"}) {
        const ReadResult r =
            ReadSchema(OneType(kPosition, std::string("\"type\": ") + good + ", \"name\": \"e\""));
        TESSERA_CHECK(r.file.has_value());
    }
}

TESSERA_TEST(file_rejects_repeated_type_number) {
    const std::string text = "{\"format_version\": 1, \"types\": [\n"
                             "{\"type\": 1, \"name\": \"a\", \"properties\": [" +
                             kPosition +
                             "]},\n"
                             "{\"type\": 1, \"name\": \"b\", \"properties\": [" +
                             kPosition + "]}]}";
    const ReadResult r = ReadSchema(text);
    TESSERA_CHECK(!r.file && r.diagnostics.size() == 1);
    ExpectDiagnostic(r.diagnostics, 0, At(text, "1, \"name\": \"b\""),
                     "the type number 1 is already used on line 2", "its own number");
}

TESSERA_TEST(file_rejects_repeated_type_name) {
    const std::string text = "{\"format_version\": 1, \"types\": [\n"
                             "{\"type\": 1, \"name\": \"a\", \"properties\": [" +
                             kPosition +
                             "]},\n"
                             "{\"type\": 2, \"name\": \"a\", \"properties\": [" +
                             kPosition + "]}]}";
    const ReadResult r = ReadSchema(text);
    TESSERA_CHECK(!r.file && r.diagnostics.size() == 1);
    ExpectDiagnostic(r.diagnostics, 0, At(text, "\"a\"", 1), "type name \"a\" is already used on line 2",
                     "its own name");
}

TESSERA_TEST(file_rejects_repeated_property_name) {
    const std::string text =
        OneType("{\"name\": \"a\", \"kind\": \"flag\"},\n{\"name\": \"a\", \"kind\": \"flag\"}");
    const ReadResult r = ReadSchema(text);
    TESSERA_CHECK(!r.file && r.diagnostics.size() == 1);
    ExpectDiagnostic(r.diagnostics, 0, At(text, "\"a\"", 1),
                     "property name \"a\" is already used on line 2 in this type", "its own name");
    // The same name in two types is fine.
    const std::string two = "{\"format_version\": 1, \"types\": [\n"
                            "{\"type\": 1, \"name\": \"a\", \"properties\": [" +
                            kPosition +
                            "]},\n"
                            "{\"type\": 2, \"name\": \"b\", \"properties\": [" +
                            kPosition + "]}]}";
    TESSERA_CHECK(ReadSchema(two).file.has_value());
}

TESSERA_TEST(file_notes_are_plain_ascii_and_bounded) {
    const auto with_note = [](const std::string& note) {
        return OneType("{\"name\": \"a\", \"kind\": \"flag\", \"note\": \"" + note + "\"}");
    };
    TESSERA_CHECK(ReadSchema(with_note("Plain, with punctuation: 1 + 2 = 3.")).file.has_value());
    TESSERA_CHECK(ReadSchema(with_note(std::string(200, 'x'))).file.has_value());
    std::string text = with_note("caf\xC3\xA9");
    ExpectDiagnostic(ReadSchema(text).diagnostics, 0, At(text, "\"caf"), "plain ASCII",
                     "write them in plain ASCII");
    text = with_note("caf\\u00e9");
    ExpectDiagnostic(ReadSchema(text).diagnostics, 0, At(text, "\"caf"), "plain ASCII", "");
    text = with_note("two\\nlines");
    ExpectDiagnostic(ReadSchema(text).diagnostics, 0, At(text, "\"two"), "plain ASCII", "");
    // A backslash at the end of a comment joins the next line to it in C++, so it is refused wherever it is.
    text = with_note("ends with a backslash\\\\");
    ExpectDiagnostic(ReadSchema(text).diagnostics, 0, At(text, "\"ends"), "backslash", "remove");
    text = with_note("in the \\\\middle");
    ExpectDiagnostic(ReadSchema(text).diagnostics, 0, At(text, "\"in the"), "backslash", "remove");
    text = with_note(std::string(201, 'x'));
    ExpectDiagnostic(ReadSchema(text).diagnostics, 0, At(text, "\"xxx"),
                     "at most 200 characters, and this one has 201", "shorter");
    const std::string note_of_type = OneType(kPosition, "\"type\": 1, \"name\": \"e\", \"note\": 7");
    ExpectDiagnostic(ReadSchema(note_of_type).diagnostics, 0, At(note_of_type, "7"), "\"note\" must be text",
                     "");
}

TESSERA_TEST(file_needs_at_least_one_type) {
    const std::string text = "{\"format_version\": 1, \"types\": []}";
    const ReadResult r = ReadSchema(text);
    TESSERA_CHECK(!r.file && r.diagnostics.size() == 1);
    ExpectDiagnostic(r.diagnostics, 0, At(text, "[]"), "needs at least one type", "add an object");
}

TESSERA_TEST(file_collects_several_problems_in_one_pass) {
    const std::string text = "{\n\"format_version\": 1,\n\"types\": [\n"
                             "{\"type\": 300, \"name\": \"a\", \"properties\": [\n"
                             "{\"name\": \"p\", \"kind\": \"flag\", \"bits\": 1},\n"
                             "{\"name\": \"q\", \"kind\": \"mask\"}]}]}";
    const ReadResult r = ReadSchema(text);
    TESSERA_CHECK(!r.file && r.diagnostics.size() == 3);
    // In the order they are in the file.
    ExpectDiagnostic(r.diagnostics, 0, At(text, "300"), "type number 300 is out of range", "");
    ExpectDiagnostic(r.diagnostics, 1, At(text, "\"bits\""), "does not belong", "");
    ExpectDiagnostic(r.diagnostics, 2, At(text, "{\"name\": \"q\""), "needs the key \"bits\"", "");
    // And there is a limit to how many are collected.
    std::string many = "{\"format_version\": 1, \"types\": [{\"type\": 1, \"name\": \"a\", \"properties\": [";
    for (int i = 0; i < 40; ++i) {
        many += std::string(i == 0 ? "" : ",") + "{\"name\": \"n" + std::to_string(i) +
                "\", \"kind\": \"flag\", \"x\": 1}";
    }
    many += "]}]}";
    TESSERA_CHECK(ReadSchema(many).diagnostics.size() == kMaxDiagnostics);
}
