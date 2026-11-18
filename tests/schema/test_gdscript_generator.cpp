// SPDX-License-Identifier: Apache-2.0
#include "schema_test_support.hpp"
#include "tessera/schema/cpp_generator.hpp"
#include "tessera/schema/gdscript_generator.hpp"
#include "tessera/schema/generator_text.hpp"
#include "tessera/schema/schema_check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace tessera::schema;
using schema_test::Has;
using schema_test::ReadAll;
using schema_test::ReadTestFile;

namespace {

struct Generated {
    SchemaFile file;
    uint64_t hash = 0;
    std::string text;
};

SchemaFile ReadFile(const std::string& text) {
    const ReadResult read = ReadSchema(text);
    TESSERA_CHECK(read.file.has_value());
    return read.file ? *read.file : SchemaFile{};
}

std::string GenerateFrom(const SchemaFile& file, const std::string& class_name = "AllKindsSchema",
                         const std::string& source_name = "all_kinds.json") {
    const CheckResult check = Check(file);
    TESSERA_CHECK(check.schema != nullptr);
    GdScriptOptions options;
    options.class_name = class_name;
    options.source_name = source_name;
    return GenerateGdScript(file, check.hash, options);
}

// Reads, checks and generates a fixture, as the tool does.
Generated GenerateFixture(const std::string& name, const std::string& class_name) {
    Generated result;
    result.file = ReadFile(ReadTestFile("fixtures/" + name));
    const CheckResult check = Check(result.file);
    TESSERA_CHECK(check.schema != nullptr);
    result.hash = check.hash;
    GdScriptOptions options;
    options.class_name = class_name;
    options.source_name = name;
    result.text = GenerateGdScript(result.file, result.hash, options);
    return result;
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    std::istringstream stream(text);
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

size_t Find(const std::string& text, const std::string& part) {
    return text.find(part);
}

std::string Replace(std::string text, const std::string& from, const std::string& to) {
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

// Where two texts first differ, as a line and a column, for a failure message.
std::string FirstDifference(const std::string& expected, const std::string& actual) {
    size_t line = 1;
    size_t column = 1;
    for (size_t i = 0; i < expected.size() && i < actual.size(); ++i) {
        if (expected[i] != actual[i]) {
            break;
        }
        if (expected[i] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return "first difference at line " + std::to_string(line) + ", column " + std::to_string(column);
}

// How many digits a number written by FloatText has: the digits of the mantissa without the zeros in front.
size_t SignificantDigits(const std::string& text) {
    std::string mantissa = text.substr(0, text.find('e'));
    std::string digits;
    for (const char c : mantissa) {
        if (c >= '0' && c <= '9') {
            digits += c;
        }
    }
    const size_t first = digits.find_first_not_of('0');
    if (first == std::string::npos) {
        return 1;
    }
    std::string trimmed = digits.substr(first);
    // Zeros at the end are not digits that were needed: 2048.0 is four digits, and so is 1073741800.0 for the
    // float that is next to 1073741824.
    while (trimmed.size() > 1 && trimmed.back() == '0') {
        trimmed.pop_back();
    }
    return trimmed.size();
}

// The floats that GDScript reads by turning the text into a double and then into a float.
float ReadLikeTheExtension(const std::string& text) {
    return static_cast<float>(std::strtod(text.c_str(), nullptr));
}

std::vector<float> SomeFloats() {
    std::vector<float> values = {0.01f,         0.1f,   0.5f,        1.0f,     2048.0f,       1e30f,
                                 1e-30f,        3.0f,   16777216.0f, 0.001f,   1.5e-5f,       123456.789f,
                                 0.3f,          100.0f, 1e10f,       65536.0f, 3.4028235e38f, 1.17549435e-38f,
                                 4294967296.0f, 0.007f};
    std::mt19937 random(12345);
    while (values.size() < 2020) {
        const uint32_t bits = random();
        float value = 0;
        std::memcpy(&value, &bits, sizeof value);
        if (value == value && std::abs(value) <= std::numeric_limits<float>::max() && value > 0) {
            values.push_back(value);
        }
    }
    return values;
}

} // namespace

TESSERA_TEST(gd_output_starts_with_notice_class_name_and_extends) {
    const Generated g = GenerateFixture("all_kinds.json", "AllKindsSchema");
    const std::vector<std::string> lines = Lines(g.text);
    TESSERA_CHECK(lines.size() > 3);
    if (lines.size() > 3) {
        TESSERA_CHECK(lines[0] ==
                      "# Generated by tessera_schema from all_kinds.json. Do not edit: change that "
                      "file and generate again.");
        TESSERA_CHECK(lines[1] == "class_name AllKindsSchema");
        TESSERA_CHECK(lines[2] == "extends RefCounted");
    }
    // Only a plain file name goes into the notice. Anything else is replaced by a neutral text.
    for (const char* odd : {"sch\xC3\xA9ma.json", "a\\b.json", "", "line\nbreak.json"}) {
        const std::string text = GenerateFrom(g.file, "AllKindsSchema", odd);
        TESSERA_CHECK(Has(text, "# Generated by tessera_schema from the schema file. Do not edit"));
        TESSERA_CHECK(!Has(text, "a\\b"));
    }
    // The class name is whatever the caller says.
    TESSERA_CHECK(Has(GenerateFrom(g.file, "GameSchema"), "\nclass_name GameSchema\n"));
}

TESSERA_TEST(gd_output_has_one_constant_for_each_type_in_number_order) {
    const Generated g = GenerateFixture("all_kinds.json", "AllKindsSchema");
    const size_t first = Find(g.text, "const TYPE_ALL_KINDS := 1\n");
    const size_t last = Find(g.text, "const TYPE_LAST := 255\n");
    TESSERA_CHECK(first != std::string::npos && last != std::string::npos && first < last);
    const size_t add_first = Find(g.text, "_add_type(schema, TYPE_ALL_KINDS, \"all_kinds\", [");
    const size_t add_last = Find(g.text, "_add_type(schema, TYPE_LAST, \"last\", [");
    TESSERA_CHECK(add_first != std::string::npos && add_last != std::string::npos && add_first < add_last);
    // The same when the file lists the types the other way round.
    SchemaFile reversed = g.file;
    std::reverse(reversed.types.begin(), reversed.types.end());
    const std::string text = GenerateFrom(reversed);
    TESSERA_CHECK(Find(text, "const TYPE_ALL_KINDS := 1\n") < Find(text, "const TYPE_LAST := 255\n"));
    TESSERA_CHECK(Find(text, "_add_type(schema, TYPE_ALL_KINDS,") <
                  Find(text, "_add_type(schema, TYPE_LAST,"));
    TESSERA_CHECK(text == g.text);
}

TESSERA_TEST(gd_hash_is_written_as_a_signed_integer) {
    const Generated all = GenerateFixture("all_kinds.json", "AllKindsSchema");
    TESSERA_CHECK(all.hash == 0xd10fff505e92ddd4ull);
    TESSERA_CHECK(Has(all.text, "\nconst HASH: int = -3382204074482803244\n"));
    const Generated example = GenerateFixture("example.json", "ExampleSchema");
    TESSERA_CHECK(Has(example.text, "\nconst HASH: int = 339718250426401488\n"));
    GdScriptOptions options;
    options.class_name = "X";
    const SchemaFile file = all.file;
    TESSERA_CHECK(Has(GenerateGdScript(file, 0x8000000000000000ull, options),
                      "\nconst HASH: int = (-9223372036854775807 - 1)\n"));
    TESSERA_CHECK(Has(GenerateGdScript(file, 0xFFFFFFFFFFFFFFFFull, options), "\nconst HASH: int = -1\n"));
    TESSERA_CHECK(Has(GenerateGdScript(file, 0, options), "\nconst HASH: int = 0\n"));
    TESSERA_CHECK(Has(GenerateGdScript(file, 0x7FFFFFFFFFFFFFFFull, options),
                      "\nconst HASH: int = 9223372036854775807\n"));
}

TESSERA_TEST(gd_property_dictionaries_use_the_keys_add_type_takes) {
    const Generated g = GenerateFixture("all_kinds.json", "AllKindsSchema");
    const std::vector<std::string> expected = {
        "\t\t{\"name\": \"position\", \"kind\": \"position\", \"step\": 0.01, \"range\": 2048.0},  # In "
        "meters.",
        "\t\t{\"name\": \"yaw\", \"kind\": \"angle\", \"bits\": 12},",
        "\t\t{\"name\": \"u8\", \"kind\": \"integer\", \"min\": 0, \"max\": 255, \"signed\": false},",
        "\t\t{\"name\": \"u16\", \"kind\": \"integer\", \"min\": 0, \"max\": 65535, \"signed\": false},",
        "\t\t{\"name\": \"u32\", \"kind\": \"integer\", \"min\": 0, \"max\": 4294967295, \"signed\": false},",
        "\t\t{\"name\": \"s8\", \"kind\": \"integer\", \"min\": -128, \"max\": 127, \"signed\": true},",
        "\t\t{\"name\": \"s32\", \"kind\": \"integer\", \"min\": -2147483648, \"max\": 2147483647, "
        "\"signed\": true},",
        "\t\t{\"name\": \"s64\", \"kind\": \"integer\", \"min\": -2147483649, \"max\": 0, \"signed\": true},",
        "\t\t{\"name\": \"alive\", \"kind\": \"flag\"},",
        "\t\t{\"name\": \"mask8\", \"kind\": \"mask\", \"bits\": 8},",
        "\t\t{\"name\": \"mask40\", \"kind\": \"mask\", \"bits\": 40},  # Forty flags.",
        "\t\t{\"name\": \"team\", \"kind\": \"choice\", \"count\": 5},",
        "\t\t{\"name\": \"species\", \"kind\": \"choice\", \"count\": 300},",
        "\t\t{\"name\": \"ready\", \"kind\": \"flag\"},",
    };
    for (const std::string& line : expected) {
        const bool found = Has(g.text, "\n" + line + "\n");
        TESSERA_CHECK(found);
        if (!found) {
            std::printf("    missing: %s\n", line.c_str());
        }
    }
    // An integer always says whether it is signed, also when the file does not.
    TESSERA_CHECK(
        Has(g.text, "\"name\": \"u8\", \"kind\": \"integer\", \"min\": 0, \"max\": 255, \"signed\": false"));
    // Nothing else is a key: the notes are comments.
    TESSERA_CHECK(!Has(g.text, "\"note\""));
}

TESSERA_TEST(gd_float_text_reads_back_as_the_same_float) {
    for (const float value : SomeFloats()) {
        const std::string text = FloatText(value);
        const float back = ReadLikeTheExtension(text);
        TESSERA_CHECK(back == value);
        if (back != value) {
            std::printf("    %s does not read back as the float it was made from\n", text.c_str());
        }
    }
    // A schema file may say more digits than a float has: the same float comes back, from the shorter text.
    const std::string text = schema_test::OneType(
        "{\"name\": \"p\", \"kind\": \"position\", \"step\": 0.30000000000000004, \"range\": 1000.0}");
    const SchemaFile file = ReadFile(text);
    const std::string script = GenerateFrom(file, "Tiny", "tiny.json");
    TESSERA_CHECK(Has(script, "\"step\": 0.3, \"range\": 1000.0}"));
    TESSERA_CHECK(ReadLikeTheExtension("0.3") == static_cast<float>(0.30000000000000004));
    TESSERA_CHECK(FloatText(0.01f) == "0.01");
    TESSERA_CHECK(FloatText(2048.0f) == "2048.0");
}

TESSERA_TEST(gd_float_text_has_at_most_nine_digits_and_a_float_form) {
    for (const float value : SomeFloats()) {
        const std::string text = FloatText(value);
        TESSERA_CHECK(!text.empty());
        TESSERA_CHECK(SignificantDigits(text) <= 9);
        TESSERA_CHECK(text.back() != 'f' && text.back() != '.');
        TESSERA_CHECK(text.front() != '+' && text.front() != '.');
        // No zero in front that is not needed: "0.5" is right, "05" and "00.5" are not.
        TESSERA_CHECK(!(text.size() >= 2 && text[0] == '0' && text[1] >= '0' && text[1] <= '9'));
        // A text without a point and without an exponent would be read as an integer, so it has ".0".
        TESSERA_CHECK(text.find_first_of(".e") != std::string::npos);
        if (SignificantDigits(text) > 9 || text.find_first_of(".e") == std::string::npos) {
            std::printf("    for %s ...\n", text.c_str());
        }
    }
    TESSERA_CHECK(FloatText(1e30f) == "1e+30");
    TESSERA_CHECK(FloatText(1e-30f) == "1e-30");
    TESSERA_CHECK(FloatText(16777216.0f) == "16777216.0");
    TESSERA_CHECK(FloatText(3.0f) == "3.0");
    TESSERA_CHECK(FloatText(0.5f) == "0.5");
    TESSERA_CHECK(FormatFloat(0.5f) == FloatText(0.5f) + "f");
}

TESSERA_TEST(gd_int64_limits_are_written_so_godot_reads_them) {
    const Generated g = GenerateFixture("all_kinds.json", "AllKindsSchema");
    TESSERA_CHECK(Has(g.text, "\"min\": (-9223372036854775807 - 1), \"max\": 9223372036854775807, "
                              "\"signed\": true},"));
    // The literal of the smallest integer is never written: it is the minus sign and a number too big.
    TESSERA_CHECK(!Has(g.text, "-9223372036854775808"));
    TESSERA_CHECK(Int64Literal(std::numeric_limits<int64_t>::min()) == "(-9223372036854775807 - 1)");
    TESSERA_CHECK(Int64Literal(std::numeric_limits<int64_t>::max()) == "9223372036854775807");
    TESSERA_CHECK(Int64Literal(-1) == "-1");
}

TESSERA_TEST(gd_notes_become_comments_and_never_keys) {
    const Generated g = GenerateFixture("all_kinds.json", "AllKindsSchema");
    // The note of a type is the comment line above its constant.
    TESSERA_CHECK(Has(g.text, "\n# One property of every kind, and an integer of every size.\n"
                              "const TYPE_ALL_KINDS := 1\n"));
    TESSERA_CHECK(Has(g.text, "\n# The highest type number.\nconst TYPE_LAST := 255\n"));
    // The note of a property is a comment at the end of its line, after two spaces.
    TESSERA_CHECK(Has(g.text, "\"range\": 2048.0},  # In meters.\n"));
    TESSERA_CHECK(Has(g.text, "\"bits\": 40},  # Forty flags.\n"));
    TESSERA_CHECK(!Has(g.text, "\"note\""));
    for (const std::string& line : Lines(g.text)) {
        if (line.rfind("\t\t{", 0) == 0) {
            TESSERA_CHECK(line.find("note") == std::string::npos || line.find("  # ") != std::string::npos);
        }
    }
}

TESSERA_TEST(gd_create_reports_what_add_type_refuses) {
    const Generated g = GenerateFixture("all_kinds.json", "AllKindsSchema");
    TESSERA_CHECK(Has(g.text, "static func create() -> TesseraSchema:\n"));
    TESSERA_CHECK(Has(g.text, "static func _add_type(schema: TesseraSchema, type_id: int, type_name: String, "
                              "properties: Array) -> void:\n"));
    TESSERA_CHECK(Has(g.text, "\tif schema.add_type(type_id, properties) != OK:\n"));
    TESSERA_CHECK(Has(g.text, "\t\tpush_error(\"AllKindsSchema: type '%s' (%d) was refused: %s\" % "
                              "[type_name, type_id, schema.get_error()])\n"));
    // The class of the script is the one in the message.
    const std::string other = GenerateFrom(g.file, "OtherSchema");
    TESSERA_CHECK(Has(other, "push_error(\"OtherSchema: type '%s'"));
    TESSERA_CHECK(!Has(other, "AllKindsSchema"));
    // `create` makes a new schema each time and hands it back, also when a type was refused.
    TESSERA_CHECK(Has(g.text, "\tvar schema := TesseraSchema.new()\n"));
    TESSERA_CHECK(Has(g.text, "\treturn schema\n"));
}

TESSERA_TEST(gd_output_matches_the_golden_file) {
    const Generated g = GenerateFixture("all_kinds.json", "AllKindsSchema");
    const std::string golden = ReadTestFile("golden/all_kinds.gd.expected");
    TESSERA_CHECK(!golden.empty());
    const bool same = g.text == golden;
    TESSERA_CHECK(same);
    if (!same) {
        std::printf("    %s (golden %zu bytes, generated %zu bytes)\n",
                    FirstDifference(golden, g.text).c_str(), golden.size(), g.text.size());
    }
}

TESSERA_TEST(gd_output_is_identical_when_generated_twice) {
    const SchemaFile file = ReadFile(ReadTestFile("fixtures/all_kinds.json"));
    const std::string first = GenerateFrom(file);
    TESSERA_CHECK(!first.empty());
    TESSERA_CHECK(first == GenerateFrom(file));
    TESSERA_CHECK(first == GenerateFrom(ReadFile(ReadTestFile("fixtures/all_kinds.json"))));
}

TESSERA_TEST(gd_output_uses_lf_tabs_and_one_final_newline) {
    const std::string text = GenerateFrom(ReadFile(ReadTestFile("fixtures/all_kinds.json")));
    TESSERA_CHECK(text.find('\r') == std::string::npos);
    TESSERA_CHECK(!text.empty() && text.back() == '\n');
    TESSERA_CHECK(text.size() > 1 && text[text.size() - 2] != '\n');
    TESSERA_CHECK(text.find(" \n") == std::string::npos); // no line ends with a space
    // Only a tab indents: Godot does not accept the two mixed.
    for (const std::string& line : Lines(text)) {
        TESSERA_CHECK(line.empty() || line.front() != ' ');
        TESSERA_CHECK(line.empty() || line.back() != '\t');
    }
    TESSERA_CHECK(Has(text, "\n\tvar schema := TesseraSchema.new()\n"));
}

TESSERA_TEST(gd_output_does_not_depend_on_type_order_in_the_file) {
    SchemaFile file = ReadFile(ReadTestFile("fixtures/all_kinds.json"));
    const std::string original = GenerateFrom(file);
    std::reverse(file.types.begin(), file.types.end());
    TESSERA_CHECK(GenerateFrom(file) == original);
    // The order of the properties is what they mean, so it is kept: swapping two changes the text.
    SchemaFile swapped = ReadFile(ReadTestFile("fixtures/all_kinds.json"));
    std::swap(swapped.types[0].properties[2], swapped.types[0].properties[3]);
    TESSERA_CHECK(GenerateFrom(swapped) != original);
}

TESSERA_TEST(gd_output_has_no_date_path_or_user_name) {
    schema_test::TempDir dir;
    const std::string in =
        dir.Write("some_user_folder/schemas/all_kinds.json", ReadTestFile("fixtures/all_kinds.json"));
    // The tool gives the generator the name of the file, with no folder.
    const std::string text =
        GenerateFrom(ReadFile(ReadAll(in)), "Stable", std::filesystem::path(in).filename().string());
    TESSERA_CHECK(!text.empty());
    TESSERA_CHECK(Has(text, "# Generated by tessera_schema from all_kinds.json. Do not edit"));
    TESSERA_CHECK(!Has(text, "some_user_folder"));
    TESSERA_CHECK(!Has(text, dir.Path().filename().string()));
    TESSERA_CHECK(!Has(text, "schemas/"));
    TESSERA_CHECK(!Has(text, "tessera_schema_test_"));
    TESSERA_CHECK(!Has(text, "__DATE__") && !Has(text, "__TIME__"));
    TESSERA_CHECK(!schema_test::HasDateOrTime(text));
    TESSERA_CHECK(!Has(text, "UTC") && !Has(text, "Generated on") && !Has(text, "Generated at"));
    // The check itself: it sees a date and a time, and not the digits of a big number.
    TESSERA_CHECK(schema_test::HasDateOrTime("made 2036-11-18") && schema_test::HasDateOrTime("at 12:34:56"));
    TESSERA_CHECK(!schema_test::HasDateOrTime(
        "const HASH: int = 9223372036854775807 and 2036 and -3382204074482803244"));
}

TESSERA_TEST(gd_output_is_the_same_for_an_input_with_crlf_and_a_bom) {
    const std::string lf = ReadTestFile("fixtures/all_kinds.json");
    const std::string crlf = "\xEF\xBB\xBF" + Replace(lf, "\n", "\r\n");
    TESSERA_CHECK(crlf.find("\r\n") != std::string::npos);
    TESSERA_CHECK(GenerateFrom(ReadFile(crlf)) == GenerateFrom(ReadFile(lf)));
}
