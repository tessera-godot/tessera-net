// SPDX-License-Identifier: Apache-2.0
#include "tessera/schema/names.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <utility>

namespace tessera::schema {

namespace {

// The keywords of C++20, with the alternative operator names. `final`, `override`, `import` and `module` are
// left out: they only mean something in some places, and `int final = 0;` compiles with MSVC, GCC and Clang.
const std::vector<std::string> kCppReserved = {
    "alignas",     "alignof",  "and",        "and_eq",    "asm",       "auto",         "bitand",
    "bitor",       "bool",     "break",      "case",      "catch",     "char",         "char16_t",
    "char32_t",    "char8_t",  "class",      "co_await",  "co_return", "co_yield",     "compl",
    "concept",     "const",    "const_cast", "consteval", "constexpr", "constinit",    "continue",
    "decltype",    "default",  "delete",     "do",        "double",    "dynamic_cast", "else",
    "enum",        "explicit", "export",     "extern",    "false",     "float",        "for",
    "friend",      "goto",     "if",         "inline",    "int",       "long",         "mutable",
    "namespace",   "new",      "noexcept",   "not",       "not_eq",    "nullptr",      "operator",
    "or",          "or_eq",    "private",    "protected", "public",    "register",     "reinterpret_cast",
    "requires",    "return",   "short",      "signed",    "sizeof",    "static",       "static_assert",
    "static_cast", "struct",   "switch",     "template",  "this",      "thread_local", "throw",
    "true",        "try",      "typedef",    "typeid",    "typename",  "union",        "unsigned",
    "using",       "virtual",  "void",       "volatile",  "wchar_t",   "while",        "xor",
    "xor_eq",
};

// The keywords of GDScript in Godot 4.7.2, from the table KEYWORDS in
// modules/gdscript/gdscript_tokenizer.cpp. The four written in capitals (INF, NAN, PI, TAU) are left out: a
// name cannot have capitals.
const std::vector<std::string> kGdScriptReserved = {
    "as",       "and",    "assert",    "await", "break",   "breakpoint", "class",   "class_name", "const",
    "continue", "elif",   "else",      "enum",  "extends", "for",        "func",    "if",         "in",
    "is",       "match",  "namespace", "not",   "or",      "pass",       "preload", "return",     "self",
    "signal",   "static", "super",     "trait", "var",     "void",       "while",   "when",       "yield",
};

// Names that become macros: `far`, `near`, `min`, `max`, `small`, `interface`, `pascal`, `cdecl` and `hyper` in the headers of
// the Windows SDK (minwindef.h, ntdef.h, rpcndr.h, combaseapi.h, basetyps.h), `errno`, `stdin`, `stdout` and `stderr` in the C
// library of Windows (and `errno`, `math_errhandling`, `sched_priority` in glibc), `linux` and `unix` in GCC and Clang when the
// language is the GNU one. `min` and `max` are macros that take arguments, so only a use like `min(` is caught. Found by
// reading the SDK headers and by listing the macros of the standard headers the generated code includes.
const std::vector<std::string> kMacroNames = {"cdecl",  "errno", "far",   "hyper",  "interface",        "linux",
                                              "math_errhandling", "max",   "min",   "near",   "pascal",
                                              "sched_priority",   "small", "stderr", "stdin", "stdout", "unix"};

// The types the generated columns are made of. A property with one of these names would hide the type from the columns that
// come after it.
const std::vector<std::string> kColumnTypeNames = {"int16_t", "int32_t", "int64_t", "int8_t",
                                                   "uint16_t", "uint32_t", "uint64_t", "uint8_t"};

bool Contains(const std::vector<std::string>& list, std::string_view name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}

bool IsLower(char c) {
    return c >= 'a' && c <= 'z';
}

bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

// [a-z][a-z0-9]*(_[a-z][a-z0-9]*)*
bool HasValidShape(std::string_view name) {
    bool part_start = true;
    for (const char c : name) {
        if (part_start) {
            if (!IsLower(c)) {
                return false;
            }
            part_start = false;
        } else if (c == '_') {
            part_start = true;
        } else if (!IsLower(c) && !IsDigit(c)) {
            return false;
        }
    }
    return !part_start;
}

std::string Quoted(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

} // namespace

const std::vector<std::string>& CppReservedWords() {
    return kCppReserved;
}

const std::vector<std::string>& GdScriptReservedWords() {
    return kGdScriptReserved;
}

const std::vector<std::string>& MacroNames() {
    return kMacroNames;
}

std::string CheckName(std::string_view name, std::string* fix) {
    const auto set_fix = [fix](std::string text) {
        if (fix != nullptr) {
            *fix = std::move(text);
        }
    };
    set_fix({});
    if (name.empty()) {
        set_fix("give it a name made of lowercase letters, digits and underscores, like \"max_health\"");
        return "the name is empty";
    }
    if (name.size() > kMaxNameLength) {
        set_fix("use at most " + std::to_string(kMaxNameLength) + " characters");
        return "the name " + Quoted(name) + " has " + std::to_string(name.size()) +
               " characters, and the limit is " + std::to_string(kMaxNameLength);
    }
    if (!HasValidShape(name)) {
        set_fix(
            "use lowercase letters, digits and underscores, and start each part with a letter, for example "
            "\"max_health\"");
        return Quoted(name) + " is not a valid name";
    }
    const bool cpp = Contains(kCppReserved, name);
    const bool gdscript = Contains(kGdScriptReserved, name);
    if (cpp || gdscript) {
        set_fix("use another name; adding a word to it, as in " + Quoted(std::string(name) + "_id") +
                ", works");
        return Quoted(name) + " is a keyword in " +
               (cpp && gdscript ? "C++ and in GDScript"
                : cpp           ? "C++"
                                : "GDScript");
    }
    if (Contains(kColumnTypeNames, name)) {
        set_fix("use another name; adding a word to it, as in " + Quoted(std::string(name) + "_value") + ", works");
        return Quoted(name) + " is the name of a type that the columns of the generated code are made of";
    }
    if (Contains(kMacroNames, name)) {
        set_fix("use another name; adding a word to it, as in " + Quoted(std::string(name) + "_value") +
                ", works");
        return Quoted(name) + " is a macro in some C++ headers (Windows, or GCC in its GNU mode) and would "
                              "break the code that is generated";
    }
    return {};
}

std::string GeneratedNames::PascalCase(std::string_view name) {
    std::string result;
    bool start = true;
    for (const char c : name) {
        if (c == '_') {
            start = true;
        } else if (start && IsLower(c)) {
            result += static_cast<char>(c - 'a' + 'A');
            start = false;
        } else {
            result += c;
            start = false;
        }
    }
    return result;
}

std::string GeneratedNames::ColumnsStruct(std::string_view type_name) {
    return PascalCase(type_name) + "Columns";
}

std::string GeneratedNames::TypeConstant(std::string_view type_name) {
    return "k" + PascalCase(type_name) + "Type";
}

std::string GeneratedNames::AddFunction(std::string_view type_name) {
    return "Add" + PascalCase(type_name) + "Type";
}

std::string GeneratedNames::MakeFunction(std::string_view type_name) {
    return "Make" + PascalCase(type_name) + "Table";
}

std::string GeneratedNames::PropertyConstant(std::string_view type_name, std::string_view property_name,
                                             std::string_view suffix) {
    return "k" + PascalCase(type_name) + PascalCase(property_name) + std::string(suffix);
}

std::vector<std::string> GeneratedNames::ColumnMembers(const PropertyDecl& property) {
    if (property.kind == Kind::Position) {
        return {property.name + "_x", property.name + "_y", property.name + "_z"};
    }
    return {property.name};
}

std::vector<std::string> GeneratedNames::ConstantSuffixes(Kind kind) {
    switch (kind) {
    case Kind::Position:
        return {"Step", "Range"};
    case Kind::Angle:
    case Kind::Mask:
        return {"Bits"};
    case Kind::Integer:
        return {"Min", "Max"};
    case Kind::Choice:
        return {"Count"};
    case Kind::Flag:
        return {};
    }
    return {};
}

std::vector<Diagnostic> FindCollisions(const SchemaFile& file) {
    std::vector<Diagnostic> found;
    // Names in the scope of the generated namespace: who made each one, and where.
    std::map<std::string, std::vector<json::Position>> global;
    const auto add_global = [&global](const std::string& name, json::Position where) {
        global[name].push_back(where);
    };
    add_global("kSchemaHash", json::Position{0, 0});
    add_global("AddGeneratedTypes", json::Position{0, 0});
    for (const TypeDecl& type : file.types) {
        if (!CheckName(type.name).empty()) {
            continue;
        }
        add_global(GeneratedNames::TypeConstant(type.name), type.name_where.position);
        add_global(GeneratedNames::ColumnsStruct(type.name), type.name_where.position);
        add_global(GeneratedNames::AddFunction(type.name), type.name_where.position);
        add_global(GeneratedNames::MakeFunction(type.name), type.name_where.position);
        // The members of the column struct of this type.
        std::map<std::string, json::Position> members;
        for (const PropertyDecl& property : type.properties) {
            if (!CheckName(property.name).empty()) {
                continue;
            }
            for (const std::string& suffix : GeneratedNames::ConstantSuffixes(property.kind)) {
                add_global(GeneratedNames::PropertyConstant(type.name, property.name, suffix),
                           property.name_where.position);
            }
            for (const std::string& member : GeneratedNames::ColumnMembers(property)) {
                const auto [place, inserted] = members.emplace(member, property.name_where.position);
                if (!inserted) {
                    found.push_back(Diagnostic{property.name_where.position,
                                               "the column " + Quoted(member) +
                                                   " of this property is also made by the property on line " +
                                                   std::to_string(place->second.line),
                                               "rename one of the two properties"});
                }
            }
        }
    }
    std::set<std::pair<uint32_t, uint32_t>> reported; // one diagnostic for each name in the file
    for (const auto& [name, places] : global) {
        if (places.size() < 2) {
            continue;
        }
        for (const json::Position& place : places) {
            if (place.line == 0 || !reported.insert({place.line, place.column}).second) {
                continue;
            }
            found.push_back(Diagnostic{place,
                                       "this name makes " + Quoted(name) +
                                           " in the generated C++, and another name makes it too",
                                       "rename one of the names that make it"});
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return a.position.line != b.position.line ? a.position.line < b.position.line
                                                  : a.position.column < b.position.column;
    });
    return found;
}

} // namespace tessera::schema
