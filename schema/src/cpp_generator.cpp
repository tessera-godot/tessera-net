// SPDX-License-Identifier: Apache-2.0
#include "tessera/schema/cpp_generator.hpp"

#include "tessera/schema/columns.hpp"
#include "tessera/schema/names.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>

namespace tessera::schema {

namespace {

std::string Hex16(uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "0x%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::string Int64Literal(int64_t value) {
    if (value == std::numeric_limits<int64_t>::min()) {
        return "(-9223372036854775807 - 1)"; // the literal -9223372036854775808 does not compile
    }
    return std::to_string(value);
}

// The first lines of a block, and so on: each line is built here and joined with "\n" at the end.
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

std::string ColumnType(const PropertyDecl& property) {
    return ColumnFor(property).cpp_type;
}

void WriteConstants(Writer& w, const TypeDecl& type) {
    w.Line("inline constexpr uint32_t " + GeneratedNames::TypeConstant(type.name) + " = " + std::to_string(type.type) +
           ";");
    for (const PropertyDecl& p : type.properties) {
        const auto name = [&](const char* suffix) { return GeneratedNames::PropertyConstant(type.name, p.name, suffix); };
        switch (p.kind) {
        case Kind::Position:
            w.Line("inline constexpr float " + name("Step") + " = " + FormatFloat(static_cast<float>(p.step)) + ";");
            w.Line("inline constexpr float " + name("Range") + " = " + FormatFloat(static_cast<float>(p.range)) + ";");
            break;
        case Kind::Angle:
        case Kind::Mask:
            w.Line("inline constexpr uint32_t " + name("Bits") + " = " + std::to_string(p.bits) + ";");
            break;
        case Kind::Integer:
            w.Line("inline constexpr int64_t " + name("Min") + " = " + Int64Literal(p.min_value) + ";");
            w.Line("inline constexpr int64_t " + name("Max") + " = " + Int64Literal(p.max_value) + ";");
            break;
        case Kind::Choice:
            w.Line("inline constexpr uint32_t " + name("Count") + " = " + std::to_string(p.count) + ";");
            break;
        case Kind::Flag:
            break;
        }
    }
}

void WriteColumns(Writer& w, const TypeDecl& type) {
    w.Line("struct " + GeneratedNames::ColumnsStruct(type.name) + " {");
    for (const PropertyDecl& p : type.properties) {
        if (!p.note.empty()) {
            w.Line("    // " + p.note);
        }
        for (const std::string& member : GeneratedNames::ColumnMembers(p)) {
            w.Line("    " + ColumnType(p) + " " + member + "[tessera::net::kPageSlots];");
        }
    }
    w.Line("};");
}

// One call that makes the descriptor of a property: on one line when it fits in 120 columns, and one argument to a
// line when it does not.
std::string Call(const std::string& indent, const std::string& function, const std::vector<std::string>& args) {
    std::string one_line = indent + function + "(";
    for (size_t i = 0; i < args.size(); ++i) {
        one_line += (i == 0 ? "" : ", ") + args[i];
    }
    one_line += "),";
    if (one_line.size() <= 120) {
        return one_line;
    }
    std::string text = indent + function + "(";
    for (size_t i = 0; i < args.size(); ++i) {
        text += "\n" + indent + "    " + args[i] + (i + 1 < args.size() ? "," : "),");
    }
    return text;
}

std::string Descriptor(const std::string& indent, const TypeDecl& type, const PropertyDecl& p) {
    const std::string columns = GeneratedNames::ColumnsStruct(type.name);
    const auto column = [&](const std::string& member) { return "TESSERA_COLUMN(" + columns + ", " + member + ")"; };
    const auto constant = [&](const char* suffix) {
        return GeneratedNames::PropertyConstant(type.name, p.name, suffix);
    };
    switch (p.kind) {
    case Kind::Position:
        return Call(indent, "PropertyDescriptor::Position",
                    {column(p.name + "_x"), column(p.name + "_y"), column(p.name + "_z"), constant("Step"),
                     constant("Range")});
    case Kind::Angle:
        return Call(indent, "PropertyDescriptor::Angle", {column(p.name), constant("Bits")});
    case Kind::Integer:
        return Call(indent, "PropertyDescriptor::Integer",
                    {column(p.name), p.is_signed ? "true" : "false", constant("Min"), constant("Max")});
    case Kind::Flag:
        return Call(indent, "PropertyDescriptor::Bits", {column(p.name), "1"});
    case Kind::Mask:
        return Call(indent, "PropertyDescriptor::Bits", {column(p.name), constant("Bits")});
    case Kind::Choice:
        return Call(indent, "PropertyDescriptor::Enum", {column(p.name), constant("Count")});
    }
    return {};
}

void WriteAddFunction(Writer& w, const TypeDecl& type) {
    w.Line("inline bool " + GeneratedNames::AddFunction(type.name) +
           "(tessera::net::ReplicationSchema& schema, std::string* error = nullptr) {");
    w.Line("    using tessera::net::PropertyDescriptor;");
    w.Line("    return schema.AddType(");
    w.Line("        " + GeneratedNames::TypeConstant(type.name) + ",");
    w.Line("        {");
    for (const PropertyDecl& p : type.properties) {
        w.Line(Descriptor("            ", type, p));
    }
    w.Line("        },");
    w.Line("        error);");
    w.Line("}");
}

void WriteMakeFunction(Writer& w, const TypeDecl& type) {
    const std::string columns = GeneratedNames::ColumnsStruct(type.name);
    w.Line("/// Throws std::invalid_argument when `shard` does not fit in an entity id (it is at most 4095).");
    w.Line("inline std::unique_ptr<tessera::net::EntityTable<" + columns + ">> " +
           GeneratedNames::MakeFunction(type.name) + "(uint32_t shard) {");
    w.Line("    return std::make_unique<tessera::net::EntityTable<" + columns + ">>(");
    w.Line("        tessera::net::TableConfig{shard, " + GeneratedNames::TypeConstant(type.name) + "});");
    w.Line("}");
}

} // namespace

bool IsValidNamespace(std::string_view name, std::string* why) {
    const auto refuse = [why](std::string text) {
        if (why != nullptr) {
            *why = std::move(text);
        }
        return false;
    };
    if (name.empty()) {
        return refuse("the namespace is empty");
    }
    size_t start = 0;
    while (start <= name.size()) {
        const size_t end = name.find("::", start);
        const std::string_view part = name.substr(start, end == std::string_view::npos ? end : end - start);
        if (part.empty()) {
            return refuse("\"" + std::string(name) + "\" has an empty part; write names separated by ::, like game::schema");
        }
        const auto is_start = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
        const auto is_part = [&](char c) { return is_start(c) || (c >= '0' && c <= '9'); };
        if (!is_start(part.front()) || !std::all_of(part.begin(), part.end(), is_part)) {
            return refuse("\"" + std::string(part) + "\" is not a valid C++ name: use letters, digits and underscores, "
                          "and do not start with a digit");
        }
        const auto& keywords = CppReservedWords();
        if (std::find(keywords.begin(), keywords.end(), part) != keywords.end()) {
            return refuse("\"" + std::string(part) + "\" is a C++ keyword and cannot be a namespace");
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 2;
    }
    return true;
}

std::string FormatFloat(float value) {
    char buffer[64];
    for (int digits = 1; digits <= 9; ++digits) {
        std::snprintf(buffer, sizeof buffer, "%.*g", digits, static_cast<double>(value));
        if (std::strtof(buffer, nullptr) == value) {
            break;
        }
    }
    std::string text = buffer;
    if (text.find_first_of(".eEn") == std::string::npos) { // no point, exponent, or inf and nan
        text += ".0";
    }
    return text + "f";
}

std::string GenerateCpp(const SchemaFile& file, uint64_t hash, const CppOptions& options) {
    std::vector<const TypeDecl*> types;
    for (const TypeDecl& type : file.types) {
        types.push_back(&type);
    }
    std::stable_sort(types.begin(), types.end(), [](const TypeDecl* a, const TypeDecl* b) { return a->type < b->type; });

    Writer w;
    w.Line("// Generated by tessera_schema from " + options.source_name +
           ". Do not edit: change that file and generate again.");
    w.Line("#pragma once");
    w.Line();
    w.Line("#include \"tessera/net/entity_table.hpp\"");
    w.Line("#include \"tessera/net/replication/property_schema.hpp\"");
    w.Line();
    w.Line("#include <cstddef>");
    w.Line("#include <cstdint>");
    w.Line("#include <memory>");
    w.Line("#include <string>");
    w.Line("#include <utility>");
    w.Line();
    w.Line("namespace " + options.ns + " {");
    w.Line();
    w.Line("/// The fingerprint of the whole schema file. A schema made of only some of the types has another one.");
    w.Line("inline constexpr uint64_t kSchemaHash = " + Hex16(hash) + "ull;");
    for (const TypeDecl* type : types) {
        w.Line();
        if (!type->note.empty()) {
            w.Line("// " + type->note);
        }
        WriteConstants(w, *type);
        w.Line();
        WriteColumns(w, *type);
        w.Line();
        WriteAddFunction(w, *type);
        w.Line();
        WriteMakeFunction(w, *type);
    }
    w.Line();
    w.Line("/// Adds every type of the file, in the order of their numbers, and stops at the first one that fails.");
    w.Line("inline bool AddGeneratedTypes(tessera::net::ReplicationSchema& schema, std::string* error = nullptr) {");
    std::string chain;
    for (size_t i = 0; i < types.size(); ++i) {
        chain += (i == 0 ? "    return " : " &&\n           ") + GeneratedNames::AddFunction(types[i]->name) +
                 "(schema, error)";
    }
    w.Line(chain + ";");
    w.Line("}");
    w.Line();
    w.Line("} // namespace " + options.ns);
    return w.Take();
}

} // namespace tessera::schema
