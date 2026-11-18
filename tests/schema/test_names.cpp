// SPDX-License-Identifier: Apache-2.0
#include "schema_test_support.hpp"
#include "tessera/schema/names.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace tessera::schema;
using schema_test::At;
using schema_test::ExpectDiagnostic;
using schema_test::Has;
using schema_test::OneType;

namespace {

bool Contains(const std::vector<std::string>& list, const std::string& word) {
    return std::find(list.begin(), list.end(), word) != list.end();
}

// A name is refused with a message, and (when asked) with a correction.
bool Refused(const std::string& name) {
    std::string fix;
    const std::string message = CheckName(name, &fix);
    return !message.empty() && !fix.empty();
}

} // namespace

TESSERA_TEST(names_accept_lowercase_with_underscores) {
    for (const char* name :
         {"entity", "max_health", "a", "a1", "player_one", "x2_y3", "position", "ab_cd_ef"}) {
        TESSERA_CHECK(CheckName(name).empty());
    }
    TESSERA_CHECK(CheckName(std::string(48, 'a')).empty());
}

TESSERA_TEST(names_reject_uppercase_digits_first_and_double_underscore) {
    for (const char* name :
         {"Player", "playerOne", "1a", "a__b", "a_", "_a", "a_1", "", "a-b", "a b", "\xC3\xA9"}) {
        TESSERA_CHECK(Refused(name));
    }
    TESSERA_CHECK(Refused(std::string(49, 'a')));
    std::string fix;
    TESSERA_CHECK(Has(CheckName("Player", &fix), "\"Player\" is not a valid name"));
    TESSERA_CHECK(Has(fix, "lowercase letters, digits and underscores"));
    TESSERA_CHECK(Has(fix, "\"max_health\""));
    TESSERA_CHECK(Has(CheckName(std::string(49, 'a'), &fix), "49 characters, and the limit is 48"));
    // The reading of a file points at the name.
    const std::string text = OneType("{\"name\": \"Player\", \"kind\": \"flag\"}");
    const ReadResult read = ReadSchema(text);
    TESSERA_CHECK(!read.file.has_value());
    ExpectDiagnostic(read.diagnostics, 0, At(text, "\"Player\""), "\"Player\" is not a valid name",
                     "start each part with a letter");
    const std::string type_text =
        OneType("{\"name\": \"p\", \"kind\": \"flag\"}", "\"type\": 1, \"name\": \"1st\"");
    ExpectDiagnostic(ReadSchema(type_text).diagnostics, 0, At(type_text, "\"1st\""), "is not a valid name",
                     "");
}

TESSERA_TEST(names_reject_cpp_keywords) {
    const std::vector<std::string>& words = CppReservedWords();
    for (const std::string& word : words) {
        TESSERA_CHECK(Refused(word));
        TESSERA_CHECK(Has(CheckName(word), "keyword in C++"));
    }
    for (const char* word : {"class", "int", "default", "new", "and", "namespace", "template", "operator",
                             "xor_eq", "co_await"}) {
        TESSERA_CHECK(Contains(words, word));
    }
    // These only mean something in some places: `int final = 0;` compiles with MSVC, GCC and Clang.
    for (const char* word : {"final", "override", "import", "module"}) {
        TESSERA_CHECK(!Contains(words, word));
        TESSERA_CHECK(CheckName(word).empty());
    }
    std::string fix;
    CheckName("class", &fix);
    TESSERA_CHECK(Has(fix, "another name") && Has(fix, "\"class_id\""));
}

TESSERA_TEST(names_reject_gdscript_keywords) {
    const std::vector<std::string>& words = GdScriptReservedWords();
    for (const std::string& word : words) {
        TESSERA_CHECK(Refused(word));
        TESSERA_CHECK(Has(CheckName(word), "GDScript"));
    }
    for (const char* word :
         {"func", "class_name", "signal", "var", "extends", "match", "pass", "await", "preload", "null"}) {
        TESSERA_CHECK(Contains(words, word));
    }
    TESSERA_CHECK(Has(CheckName("func"), "keyword in GDScript"));
    // `null` is a literal that the tokenizer of GDScript reads outside its table of keywords (as `true` and
    // `false`, which C++ already refuses); INF, NAN, PI and TAU are constants, and capitals are not allowed
    // in a name anyway.
    TESSERA_CHECK(Has(CheckName("null"), "GDScript"));
    TESSERA_CHECK(Has(CheckName("class"), "C++ and in GDScript")); // in both lists
}

TESSERA_TEST(names_reject_windows_macro_names) {
    const std::vector<std::string>& names = MacroNames();
    for (const char* name :
         {"min", "max", "near", "far", "small", "interface", "linux", "unix", "errno", "stdin", "stdout",
          "stderr", "pascal", "cdecl", "hyper", "math_errhandling", "sched_priority"}) {
        TESSERA_CHECK(Contains(names, name));
        TESSERA_CHECK(Refused(name));
        TESSERA_CHECK(Has(CheckName(name), "macro"));
    }
    for (const std::string& name : names) {
        TESSERA_CHECK(Refused(name));
    }
}

TESSERA_TEST(names_reject_the_fixed_width_integer_types_the_columns_use) {
    for (const char* name :
         {"int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t"}) {
        TESSERA_CHECK(Refused(name));
        TESSERA_CHECK(Has(CheckName(name), "type"));
    }
    // Names that look alike but are not types of the columns are fine, and so are the other standard names:
    // the generated header was compiled with them (see fixtures/risky_names.json and
    // test_generated_header.cpp).
    for (const char* name : {"int8", "uint", "size_t", "byte", "string", "move", "net", "std", "tessera"}) {
        TESSERA_CHECK(CheckName(name).empty());
    }
}

TESSERA_TEST(names_that_collide_after_conversion_are_rejected) {
    // Type "a" with property "b_c" and type "a_b" with property "c" both make kABCStep and kABCRange.
    const std::string position = ", \"kind\": \"position\", \"step\": 1, \"range\": 10}";
    const std::string text = "{\"format_version\": 1, \"types\": [\n"
                             "{\"type\": 1, \"name\": \"a\", \"properties\": [{\"name\": \"b_c\"" +
                             position +
                             "]},\n"
                             "{\"type\": 2, \"name\": \"a_b\", \"properties\": [{\"name\": \"c\"" +
                             position + "]}\n]}";
    const ReadResult read = ReadSchema(text);
    TESSERA_CHECK(!read.file.has_value());
    // Not only the properties: kAType is made by "a", and kABType by "a_b", so those are fine; the two that
    // clash are the constants of the properties, and each place is reported once (for Step and for Range).
    bool first = false;
    bool second = false;
    for (const Diagnostic& d : read.diagnostics) {
        TESSERA_CHECK(Has(d.message, "\"kABC"));
        first = first || (d.position.line == At(text, "\"b_c\"").line &&
                          d.position.column == At(text, "\"b_c\"").column);
        second = second ||
                 (d.position.line == At(text, "\"c\"").line && d.position.column == At(text, "\"c\"").column);
    }
    TESSERA_CHECK(first && second);
    // Names that only look alike do not clash.
    const std::string fine = "{\"format_version\": 1, \"types\": [\n"
                             "{\"type\": 1, \"name\": \"a\", \"properties\": [{\"name\": \"b_c\"" +
                             position +
                             "]},\n"
                             "{\"type\": 2, \"name\": \"a_b\", \"properties\": [{\"name\": \"d\"" +
                             position + "]}\n]}";
    TESSERA_CHECK(ReadSchema(fine).file.has_value());
}

TESSERA_TEST(names_column_collision_between_position_axes_and_a_property) {
    const std::string text =
        OneType("{\"name\": \"position\", \"kind\": \"position\", \"step\": 1, \"range\": 10},\n"
                "{\"name\": \"position_x\", \"kind\": \"flag\"}");
    const ReadResult read = ReadSchema(text);
    TESSERA_CHECK(!read.file.has_value());
    TESSERA_CHECK(read.diagnostics.size() == 1);
    ExpectDiagnostic(read.diagnostics, 0, At(text, "\"position_x\""), "column \"position_x\"", "rename");
    // The constants of a type and the fixed names of the generated header cannot be taken either: the type
    // "schema_hash" makes kSchemaHashType, which is fine, but a type "generated" makes AddGeneratedType, not
    // AddGeneratedTypes.
    TESSERA_CHECK(GeneratedNames::AddFunction("generated") == "AddGeneratedType");
    TESSERA_CHECK((GeneratedNames::ColumnMembers(
                       PropertyDecl{"pos", Kind::Position, 1, 1, 0, 0, false, 0, 0, {}, {}, {}}) ==
                   std::vector<std::string>{"pos_x", "pos_y", "pos_z"}));
}

TESSERA_TEST(names_pascal_case_is_injective_for_valid_names) {
    TESSERA_CHECK(GeneratedNames::PascalCase("player_one") == "PlayerOne");
    TESSERA_CHECK(GeneratedNames::PascalCase("a1") == "A1");
    TESSERA_CHECK(GeneratedNames::PascalCase("x2_y3") == "X2Y3");
    TESSERA_CHECK(GeneratedNames::ColumnsStruct("player") == "PlayerColumns");
    TESSERA_CHECK(GeneratedNames::TypeConstant("player") == "kPlayerType");
    TESSERA_CHECK(GeneratedNames::AddFunction("player") == "AddPlayerType");
    TESSERA_CHECK(GeneratedNames::MakeFunction("player") == "MakePlayerTable");
    TESSERA_CHECK(GeneratedNames::PropertyConstant("player", "owner", "Max") == "kPlayerOwnerMax");
    // Every valid name made of up to four short parts: no two different names give the same result.
    const std::vector<std::string> parts = {"a", "b", "a1", "ab"};
    std::vector<std::string> names = {};
    std::vector<std::string> level = {""};
    for (int length = 1; length <= 4; ++length) {
        std::vector<std::string> next;
        for (const std::string& stem : level) {
            for (const std::string& part : parts) {
                next.push_back(stem.empty() ? part : stem + "_" + part);
            }
        }
        names.insert(names.end(), next.begin(), next.end());
        level = next;
    }
    TESSERA_CHECK(names.size() == 4 + 16 + 64 + 256);
    std::set<std::string> seen;
    for (const std::string& name : names) {
        TESSERA_CHECK(CheckName(name).empty());
        TESSERA_CHECK(seen.insert(GeneratedNames::PascalCase(name)).second);
    }
}
