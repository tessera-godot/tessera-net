// SPDX-License-Identifier: Apache-2.0
// The name of the class that the GDScript output declares has to be one that Godot lets a script take.
#include "schema_test_support.hpp"
#include "tessera/schema/names.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace tessera::schema;
using schema_test::Has;

namespace {

bool Refused(const std::string& name) {
    return !CheckGdScriptClassName(name).empty();
}

// A capital letter, then letters and digits: the shape of every name in the list of names that Godot uses.
bool IsPascalShape(std::string_view name) {
    if (name.empty() || name.front() < 'A' || name.front() > 'Z') {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    });
}

const char* const kShapeFix = "use letters and digits, start with a capital letter, and have at least one "
                              "lowercase letter, for example \"GameSchema\"";

} // namespace

TESSERA_TEST(gd_class_names_accept_pascal_case) {
    for (const char* name : {"GameSchema", "ExampleSchema", "Schema", "World", "Player2", "A1b"}) {
        TESSERA_CHECK(CheckGdScriptClassName(name).empty());
        if (Refused(name)) {
            std::printf("    for \"%s\" ...\n", name);
        }
    }
    TESSERA_CHECK(CheckGdScriptClassName("A" + std::string(47, 'a')).empty()); // exactly 48 characters
}

TESSERA_TEST(gd_class_names_reject_a_wrong_shape) {
    const std::vector<std::string> wrong = {"",
                                            "A" + std::string(48, 'a'),
                                            "schema",
                                            "game_schema",
                                            "Game-Schema",
                                            "1Game",
                                            "_Game",
                                            "Game Schema",
                                            "Sch\xC3\xA9ma",
                                            "OK",
                                            "PI",
                                            "TAU",
                                            "INF",
                                            "NAN",
                                            "FAILED",
                                            "KEY_A",
                                            "GAME"};
    for (const std::string& name : wrong) {
        std::string fix;
        const std::string message = CheckGdScriptClassName(name, &fix);
        TESSERA_CHECK(!message.empty());
        TESSERA_CHECK(!fix.empty());
        if (message.empty() || fix.empty()) {
            std::printf("    for \"%s\" ...\n", name.c_str());
        }
    }
    std::string fix;
    TESSERA_CHECK(Has(CheckGdScriptClassName("", &fix), "the name is empty"));
    TESSERA_CHECK(Has(fix, kShapeFix));
    TESSERA_CHECK(Has(CheckGdScriptClassName("A" + std::string(48, 'a'), &fix), "49 characters"));
    TESSERA_CHECK(Has(fix, "at most 48 characters"));
    TESSERA_CHECK(
        Has(CheckGdScriptClassName("game_schema", &fix), "\"game_schema\" is not a valid class name"));
    TESSERA_CHECK(Has(fix, kShapeFix));
    TESSERA_CHECK(Has(CheckGdScriptClassName("GAME", &fix), "\"GAME\" is not a valid class name"));
    TESSERA_CHECK(Has(fix, kShapeFix));
    // A name that is not plain ASCII is shown with escapes, as the other messages of the tool do.
    TESSERA_CHECK(Has(CheckGdScriptClassName("Sch\xC3\xA9ma", &fix), "Sch\\xC3\\xA9ma"));
}

TESSERA_TEST(gd_class_names_reject_keywords_and_constants) {
    TESSERA_CHECK(!GdScriptReservedWords().empty());
    for (const std::string& word : GdScriptReservedWords()) {
        TESSERA_CHECK(Refused(word));
        std::string capital = word;
        capital[0] = static_cast<char>(capital[0] - 'a' + 'A');
        // `Class` or `Extends` is not a keyword, so it may pass; it must never be refused for being one.
        if (Refused(capital)) {
            TESSERA_CHECK(!Has(CheckGdScriptClassName(capital), "keyword"));
        }
        if (!Refused(word)) {
            std::printf("    for \"%s\" ...\n", word.c_str());
        }
    }
    for (const char* name : {"INF", "NAN", "PI", "TAU"}) {
        TESSERA_CHECK(Refused(name));
    }
}

TESSERA_TEST(gd_class_names_reject_the_names_godot_uses) {
    for (const char* name : {"Node", "Node3D", "Resource", "RefCounted", "Object", "Engine", "OS", "Time",
                             "Vector3", "String", "Array", "Dictionary", "Callable", "EditorPlugin",
                             "AStar2D", "JSON", "TesseraSchema", "TesseraClient"}) {
        TESSERA_CHECK(Refused(name));
        if (!Refused(name)) {
            std::printf("    for \"%s\" ...\n", name);
        }
    }
    // A name of a class, in the right shape: the message says so, and the correction names a good one.
    std::string fix;
    TESSERA_CHECK(CheckGdScriptClassName("Node", &fix) ==
                  "\"Node\" is the name of a class in Godot, and a script cannot take it");
    TESSERA_CHECK(fix == "use another name, for example \"NodeSchema\"");
    TESSERA_CHECK(CheckGdScriptClassName("Node3D", &fix) ==
                  "\"Node3D\" is the name of a class in Godot, and a script cannot take it");
    TESSERA_CHECK(fix == "use another name, for example \"Node3DSchema\"");
}

TESSERA_TEST(gd_class_names_cover_the_whole_godot_list) {
    const std::span<const std::string_view> names = GodotNames();
    TESSERA_CHECK(names.size() ==
                  1073); // 1071 in the extension API of Godot 4.7.2, and the 2 of this extension
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string name(names[i]);
        TESSERA_CHECK(Refused(name));
        TESSERA_CHECK(IsPascalShape(name));
        if (i > 0) {
            TESSERA_CHECK(names[i - 1] < names[i]); // in order of bytes, and no name twice
        }
        if (!Refused(name) || !IsPascalShape(name) || (i > 0 && !(names[i - 1] < names[i]))) {
            std::printf("    for \"%s\" ...\n", name.c_str());
        }
    }
    const std::set<std::string_view> unique(names.begin(), names.end());
    TESSERA_CHECK(unique.size() == names.size());
    for (const char* name :
         {"Node", "Resource", "RefCounted", "Object", "Engine", "TesseraSchema", "TesseraClient"}) {
        TESSERA_CHECK(std::binary_search(names.begin(), names.end(), std::string_view(name)));
    }
}

TESSERA_TEST(gd_class_names_do_not_refuse_close_names) {
    for (const char* name : {"NodeSchema", "ResourceSchema", "Node3DSchema", "TesseraSchemaX",
                             "Vector3Schema", "Players", "Entities"}) {
        TESSERA_CHECK(CheckGdScriptClassName(name).empty());
        if (Refused(name)) {
            std::printf("    for \"%s\" ...\n", name);
        }
    }
}
