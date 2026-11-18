// SPDX-License-Identifier: Apache-2.0
#include "tessera/schema/schema_file.hpp"

#include "tessera/net/entity_id.hpp"
#include "tessera/schema/names.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <utility>

namespace tessera::schema {

namespace {

enum Key : unsigned {
    kStep = 1,
    kRange = 2,
    kBits = 4,
    kMin = 8,
    kMax = 16,
    kSigned = 32,
    kCount = 64,
};

struct KindKeys {
    Kind kind;
    const char* name;
    unsigned required;
    unsigned optional;
};

// The keys each kind uses. This mirrors `kKindKeys` in extension/src/tessera_schema.cpp, which is what
// `TesseraSchema.add_type` accepts in GDScript, so that a property is written the same way in both places.
const KindKeys kKindKeys[] = {
    {Kind::Position, "position", kStep | kRange, 0},
    {Kind::Angle, "angle", kBits, 0},
    {Kind::Integer, "integer", kMin | kMax, kSigned},
    {Kind::Flag, "flag", 0, 0},
    {Kind::Mask, "mask", kBits, 0},
    {Kind::Choice, "choice", kCount, 0},
};

struct KeyName {
    unsigned bit;
    const char* name;
};

const KeyName kKeyNames[] = {
    {kStep, "step"}, {kRange, "range"},   {kBits, "bits"},   {kMin, "min"},
    {kMax, "max"},   {kSigned, "signed"}, {kCount, "count"},
};

constexpr size_t kMaxNoteLength = 200;

size_t EditDistance(std::string_view a, std::string_view b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) {
        row[j] = j;
    }
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0u : 1u)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

// "did you mean ...?" for a key that is close to one of the keys that are allowed, or nothing.
std::string Suggest(std::string_view key, const std::vector<std::string>& allowed) {
    const std::string* best = nullptr;
    size_t best_distance = 3;
    for (const std::string& candidate : allowed) {
        const size_t distance = EditDistance(key, candidate);
        if (distance < best_distance) {
            best_distance = distance;
            best = &candidate;
        }
    }
    return best != nullptr ? "did you mean \"" + *best + "\"?" : std::string();
}

const KindKeys* FindKind(std::string_view name) {
    for (const KindKeys& known : kKindKeys) {
        if (name == known.name) {
            return &known;
        }
    }
    return nullptr;
}

const char* KeyText(unsigned bit) {
    for (const KeyName& key : kKeyNames) {
        if (key.bit == bit) {
            return key.name;
        }
    }
    return "";
}

bool IsNumberKey(unsigned bit) {
    return bit == kStep || bit == kRange;
}

std::string Quoted(std::string_view text) {
    return "\"" + EchoText(text) + "\"";
}

class Reader {
public:
    std::vector<Diagnostic>& Diagnostics() {
        return diagnostics_;
    }

    void ReadRoot(const json::Value& root, SchemaFile& file) {
        if (root.GetType() != json::Type::Object) {
            Add(root.position,
                "the file must hold one JSON object, like { \"format_version\": 1, \"types\": [ ... ] }");
            return;
        }
        bool has_version = false;
        bool has_types = false;
        for (const json::Member& member : root.Members()) {
            if (member.key == "format_version") {
                has_version = true;
                ReadFormatVersion(member.value, file);
            } else if (member.key == "types") {
                has_types = true;
                ReadTypes(member.value, file);
            } else {
                Add(member.key_position, "unknown field " + Quoted(member.key) + " in the file",
                    Suggest(member.key, {"format_version", "types"}));
            }
        }
        if (!has_version) {
            Add(root.position, "the file needs a \"format_version\"", "add \"format_version\": 1");
        }
        if (!has_types) {
            Add(root.position, "the file needs a list of \"types\"",
                "add \"types\": [ ... ] with at least one type");
        }
    }

    void Finish() {
        std::stable_sort(
            diagnostics_.begin(), diagnostics_.end(), [](const Diagnostic& a, const Diagnostic& b) {
                return a.position.line != b.position.line ? a.position.line < b.position.line
                                                          : a.position.column < b.position.column;
            });
    }

private:
    void Add(json::Position position, std::string message, std::string fix = {}) {
        diagnostics_.push_back(Diagnostic{position, std::move(message), std::move(fix)});
    }

    void ReadFormatVersion(const json::Value& value, SchemaFile& file) {
        switch (value.GetType()) {
        case json::Type::Integer:
            if (value.AsInteger() == 1) {
                file.format_version = 1;
            } else {
                Add(value.position,
                    "this tool reads format version 1, and the file says " +
                        std::to_string(value.AsInteger()),
                    "set \"format_version\" to 1, or use a version of the tool that reads that format");
            }
            return;
        case json::Type::String:
            Add(value.position, "\"format_version\" is a number, and here it is text",
                "write 1 without quotes");
            return;
        case json::Type::Number:
            Add(value.position, "\"format_version\" must be a whole number, not a decimal one", "write 1");
            return;
        default:
            Add(value.position, "\"format_version\" must be the whole number 1",
                "write \"format_version\": 1");
            return;
        }
    }

    void ReadTypes(const json::Value& value, SchemaFile& file) {
        if (value.GetType() != json::Type::Array) {
            Add(value.position, "\"types\" must be a list", "write it as \"types\": [ { ... } ]");
            return;
        }
        if (value.Items().empty()) {
            Add(value.position, "the file needs at least one type",
                "add an object with a \"type\" number, a \"name\" and a list of \"properties\"");
            return;
        }
        for (const json::Value& item : value.Items()) {
            file.types.push_back(ReadType(item));
        }
    }

    TypeDecl ReadType(const json::Value& item) {
        TypeDecl type;
        type.where.position = item.position;
        if (item.GetType() != json::Type::Object) {
            Add(item.position, "a type must be an object, like { \"type\": 1, \"name\": \"entity\", "
                               "\"properties\": [ ... ] }");
            return type;
        }
        bool has_number = false;
        bool has_name = false;
        bool has_properties = false;
        for (const json::Member& member : item.Members()) {
            if (member.key == "type") {
                has_number = true;
                ReadTypeNumber(member.value, type);
            } else if (member.key == "name") {
                has_name = true;
                ReadTypeName(member.value, type);
            } else if (member.key == "note") {
                ReadNote(member.value, type.note);
            } else if (member.key == "properties") {
                has_properties = true;
                ReadProperties(member.value, type);
            } else {
                Add(member.key_position, "unknown field " + Quoted(member.key) + " in a type",
                    Suggest(member.key, {"type", "name", "note", "properties"}));
            }
        }
        if (!has_number) {
            Add(item.position, "a type needs a \"type\" number",
                "add \"type\": a whole number from 0 to 255");
        }
        if (!has_name) {
            Add(item.position, "a type needs a \"name\"",
                "add \"name\": \"entity\", or another name in lowercase");
        }
        if (!has_properties) {
            Add(item.position, "a type needs a list of \"properties\"", "add \"properties\": [ ... ]");
        }
        return type;
    }

    void ReadTypeNumber(const json::Value& value, TypeDecl& type) {
        const std::string fix = "use a whole number from 0 to " + std::to_string(net::EntityId::kMaxType);
        if (!value.IsInteger()) {
            Add(value.position,
                "the type number must be a whole number from 0 to " + std::to_string(net::EntityId::kMaxType),
                fix);
            return;
        }
        const int64_t number = value.AsInteger();
        if (number < 0 || number > static_cast<int64_t>(net::EntityId::kMaxType)) {
            Add(value.position,
                "the type number " + std::to_string(number) + " is out of range, it must be from 0 to " +
                    std::to_string(net::EntityId::kMaxType),
                fix);
            return;
        }
        type.type = static_cast<uint32_t>(number);
        const auto [place, inserted] = type_numbers_.emplace(type.type, value.position);
        if (!inserted) {
            Add(value.position,
                "the type number " + std::to_string(number) + " is already used on line " +
                    std::to_string(place->second.line),
                "give each type its own number");
        }
    }

    void ReadTypeName(const json::Value& value, TypeDecl& type) {
        if (value.GetType() != json::Type::String) {
            Add(value.position, "the name of a type must be text",
                "write it in double quotes, like \"entity\"");
            return;
        }
        type.name = value.AsString();
        type.name_where.position = value.position;
        CheckNameAt(value);
        const auto [place, inserted] = type_names_.emplace(type.name, value.position);
        if (!inserted) {
            Add(value.position,
                "the type name " + Quoted(type.name) + " is already used on line " +
                    std::to_string(place->second.line),
                "give each type its own name");
        }
    }

    // A name has to be good in C++ and in GDScript.
    void CheckNameAt(const json::Value& value) {
        std::string fix;
        const std::string problem = CheckName(value.AsString(), &fix);
        if (!problem.empty()) {
            Add(value.position, problem, fix);
        }
    }

    void ReadNote(const json::Value& value, std::string& out) {
        if (value.GetType() != json::Type::String) {
            Add(value.position, "a \"note\" must be text", "write it in double quotes");
            return;
        }
        const std::string& text = value.AsString();
        for (const char c : text) {
            const auto byte = static_cast<unsigned char>(c);
            if (c == '\\') {
                Add(value.position,
                    "a note cannot hold a backslash: at the end of a line it would join the next line of the "
                    "generated code "
                    "to the comment",
                    "remove the backslash");
                return;
            }
            if (byte < 0x20 || byte > 0x7E) {
                Add(value.position,
                    "a note can only hold plain ASCII text on one line (letters, digits, punctuation and "
                    "spaces)",
                    "remove the other characters, or write them in plain ASCII");
                return;
            }
        }
        if (text.size() > kMaxNoteLength) {
            Add(value.position,
                "a note can have at most " + std::to_string(kMaxNoteLength) +
                    " characters, and this one has " + std::to_string(text.size()),
                "make it shorter");
            return;
        }
        out = text;
    }

    void ReadProperties(const json::Value& value, TypeDecl& type) {
        if (value.GetType() != json::Type::Array) {
            Add(value.position, "\"properties\" must be a list", "write it as \"properties\": [ { ... } ]");
            return;
        }
        std::map<std::string, json::Position> names;
        for (const json::Value& item : value.Items()) {
            type.properties.push_back(ReadProperty(item, names));
        }
    }

    PropertyDecl ReadProperty(const json::Value& item, std::map<std::string, json::Position>& names) {
        PropertyDecl property;
        property.where.position = item.position;
        if (item.GetType() != json::Type::Object) {
            Add(item.position,
                "a property must be an object, like { \"name\": \"position\", \"kind\": \"position\", ... }");
            return property;
        }
        const KindKeys* kind = nullptr;
        if (const json::Value* kind_value = item.Find("kind")) {
            kind = ReadKind(*kind_value, property);
        } else {
            Add(item.position, "a property needs a \"kind\"", "add \"kind\": one of " + KindList());
        }
        unsigned seen = 0;
        bool has_name = false;
        for (const json::Member& member : item.Members()) {
            if (member.key == "kind") {
                continue;
            }
            if (member.key == "name") {
                has_name = true;
                ReadPropertyName(member.value, property, names);
                continue;
            }
            if (member.key == "note") {
                ReadNote(member.value, property.note);
                continue;
            }
            unsigned bit = 0;
            for (const KeyName& key : kKeyNames) {
                if (member.key == key.name) {
                    bit = key.bit;
                }
            }
            if (bit == 0) {
                Add(member.key_position, "unknown field " + Quoted(member.key) + " in a property",
                    Suggest(member.key, AllowedKeys(kind)));
            } else if (kind != nullptr) {
                if (((kind->required | kind->optional) & bit) == 0) {
                    Add(member.key_position,
                        "the key " + Quoted(member.key) + " does not belong to the kind " +
                            Quoted(kind->name),
                        KeysOfKind(*kind));
                } else {
                    seen |= bit;
                    ReadKeyValue(bit, member.value, property);
                }
            }
        }
        if (!has_name) {
            Add(item.position, "a property needs a \"name\"",
                "add \"name\": a name in lowercase, like \"position\"");
        }
        if (kind != nullptr) {
            for (const KeyName& key : kKeyNames) {
                if ((kind->required & key.bit) != 0 && (seen & key.bit) == 0) {
                    Add(item.position,
                        "a property of the kind " + Quoted(kind->name) + " needs the key " + Quoted(key.name),
                        "add " + Quoted(key.name) + ": " +
                            (IsNumberKey(key.bit) ? "a number" : "a whole number"));
                }
            }
        }
        return property;
    }

    static std::string KindList() {
        std::string list;
        for (const KindKeys& known : kKindKeys) {
            list += (list.empty() ? "" : ", ") + std::string(known.name);
        }
        return list;
    }

    static std::vector<std::string> AllowedKeys(const KindKeys* kind) {
        std::vector<std::string> keys = {"name", "kind", "note"};
        if (kind != nullptr) {
            for (const KeyName& key : kKeyNames) {
                if (((kind->required | kind->optional) & key.bit) != 0) {
                    keys.emplace_back(key.name);
                }
            }
        }
        return keys;
    }

    static std::string KeysOfKind(const KindKeys& kind) {
        std::string keys;
        for (const KeyName& key : kKeyNames) {
            if (((kind.required | kind.optional) & key.bit) != 0) {
                keys += (keys.empty() ? "" : ", ") + Quoted(key.name);
            }
        }
        return "remove it; a " + Quoted(kind.name) + " property " +
               (keys.empty() ? "takes no keys besides \"name\" and \"note\"" : "uses " + keys);
    }

    const KindKeys* ReadKind(const json::Value& value, PropertyDecl& property) {
        if (value.GetType() != json::Type::String) {
            Add(value.position, "\"kind\" must be text", "use one of " + KindList());
            return nullptr;
        }
        const KindKeys* kind = FindKind(value.AsString());
        if (kind == nullptr) {
            std::vector<std::string> names;
            for (const KindKeys& known : kKindKeys) {
                names.emplace_back(known.name);
            }
            const std::string hint = Suggest(value.AsString(), names);
            Add(value.position, "unknown kind " + Quoted(value.AsString()),
                (hint.empty() ? std::string() : hint + " ") + "Use one of " + KindList());
            return nullptr;
        }
        property.kind = kind->kind;
        return kind;
    }

    void ReadPropertyName(const json::Value& value, PropertyDecl& property,
                          std::map<std::string, json::Position>& names) {
        if (value.GetType() != json::Type::String) {
            Add(value.position, "the name of a property must be text",
                "write it in double quotes, like \"position\"");
            return;
        }
        property.name = value.AsString();
        property.name_where.position = value.position;
        CheckNameAt(value);
        const auto [place, inserted] = names.emplace(property.name, value.position);
        if (!inserted) {
            Add(value.position,
                "the property name " + Quoted(property.name) + " is already used on line " +
                    std::to_string(place->second.line) + " in this type",
                "give each property of a type its own name");
        }
    }

    void ReadKeyValue(unsigned bit, const json::Value& value, PropertyDecl& property) {
        const std::string key = KeyText(bit);
        switch (bit) {
        case kStep:
        case kRange:
            if (!value.IsNumeric()) {
                Add(value.position, "the key " + Quoted(key) + " must be a number",
                    "write a number without quotes, like 0.01");
                return;
            }
            (bit == kStep ? property.step : property.range) = value.AsNumber();
            return;
        case kSigned:
            if (value.GetType() != json::Type::Bool) {
                Add(value.position, "the key \"signed\" must be true or false",
                    "write true or false without quotes");
                return;
            }
            property.is_signed = value.AsBool();
            return;
        case kMin:
        case kMax:
            if (!value.IsInteger()) {
                Add(value.position, "the key " + Quoted(key) + " must be a whole number",
                    "write a whole number without quotes or a decimal point");
                return;
            }
            (bit == kMin ? property.min_value : property.max_value) = value.AsInteger();
            return;
        default: // bits and count
            if (!value.IsInteger() || value.AsInteger() < 0 ||
                value.AsInteger() > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
                Add(value.position, "the key " + Quoted(key) + " must be a whole number from 0 to 4294967295",
                    "write a whole number without quotes or a decimal point");
                return;
            }
            (bit == kBits ? property.bits : property.count) = static_cast<uint32_t>(value.AsInteger());
            return;
        }
    }

    std::vector<Diagnostic> diagnostics_;
    std::map<uint32_t, json::Position> type_numbers_;
    std::map<std::string, json::Position> type_names_;
};

} // namespace

std::string EchoText(std::string_view text) {
    constexpr size_t kShown = 40;
    std::string shown;
    for (size_t i = 0; i < text.size() && i < kShown; ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte >= 0x20 && byte < 0x7F) {
            shown += text[i];
        } else {
            static const char kDigits[] = "0123456789ABCDEF";
            shown += "\\x";
            shown += kDigits[byte >> 4];
            shown += kDigits[byte & 15];
        }
    }
    if (text.size() > kShown) {
        shown += "...";
    }
    return shown;
}

std::string_view KindName(Kind kind) {
    for (const KindKeys& known : kKindKeys) {
        if (known.kind == kind) {
            return known.name;
        }
    }
    return "";
}

ReadResult ReadSchema(std::string_view text) {
    ReadResult result;
    const json::ParseResult parsed = json::Parse(text);
    if (parsed.error) {
        result.diagnostics.push_back(
            Diagnostic{parsed.error->position, parsed.error->message, parsed.error->fix});
        return result;
    }
    SchemaFile file;
    Reader reader;
    reader.ReadRoot(*parsed.value, file);
    reader.Finish();
    result.diagnostics = std::move(reader.Diagnostics());
    if (result.diagnostics.empty()) {
        // Names that are good one by one can still make the same C++ name together.
        result.diagnostics = FindCollisions(file);
    }
    // The problems are put in the order of the file first, and only then cut, so that the ones kept are the
    // first ones.
    std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(),
                     [](const Diagnostic& a, const Diagnostic& b) {
                         return a.position.line != b.position.line ? a.position.line < b.position.line
                                                                   : a.position.column < b.position.column;
                     });
    if (result.diagnostics.size() > kMaxDiagnostics) {
        result.omitted = result.diagnostics.size() - kMaxDiagnostics;
        result.diagnostics.resize(kMaxDiagnostics);
    }
    if (result.diagnostics.empty()) {
        result.file = std::move(file);
    }
    return result;
}

std::string Format(std::string_view file_name, const Diagnostic& diagnostic) {
    std::string text = std::string(file_name) + ":" + std::to_string(diagnostic.position.line) + ":" +
                       std::to_string(diagnostic.position.column) + ": error: " + diagnostic.message;
    if (!diagnostic.fix.empty()) {
        text += "\n  fix: " + diagnostic.fix;
    }
    return text;
}

} // namespace tessera::schema
