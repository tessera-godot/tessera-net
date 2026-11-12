// SPDX-License-Identifier: Apache-2.0
#include "tessera_schema.hpp"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <utility>

namespace godot {

namespace {

std::string ToStd(const String& text) {
    return std::string(text.utf8().get_data());
}

String ToGodot(const std::string& text) {
    return String::utf8(text.c_str(), static_cast<int64_t>(text.size()));
}

bool IsText(const Variant& value) {
    return value.get_type() == Variant::STRING || value.get_type() == Variant::STRING_NAME;
}

bool IsNumber(const Variant& value) {
    return value.get_type() == Variant::FLOAT || value.get_type() == Variant::INT;
}

// The keys each kind uses besides "name" and "kind". A key that belongs to another kind is an error, and so is a
// missing key that has no default (the numbers the schema cannot guess).
enum KeyBit : unsigned {
    kStep = 1u << 0,
    kRange = 1u << 1,
    kBits = 1u << 2,
    kMin = 1u << 3,
    kMax = 1u << 4,
    kSigned = 1u << 5,
    kCount = 1u << 6,
};

struct KindKeys {
    const char* kind;
    unsigned required;
    unsigned optional;
};

const KindKeys kKindKeys[] = {
    {"position", kStep | kRange, 0},
    {"angle", kBits, 0},
    {"integer", kMin | kMax, kSigned},
    {"flag", 0, 0},
    {"mask", kBits, 0},
    {"choice", kCount, 0},
};

const std::pair<unsigned, const char*> kKeyNames[] = {{kStep, "step"}, {kRange, "range"}, {kBits, "bits"},
                                                      {kMin, "min"},   {kMax, "max"},     {kSigned, "signed"},
                                                      {kCount, "count"}};

// Compares the keys given with the ones the kind uses. An unknown kind is left to the schema's own message.
std::string CheckKeys(const std::string& where, const std::string& kind, unsigned given) {
    for (const KindKeys& known : kKindKeys) {
        if (kind != known.kind) {
            continue;
        }
        for (const auto& [bit, key] : kKeyNames) {
            if ((given & bit) != 0 && ((known.required | known.optional) & bit) == 0) {
                return where + ": key \"" + key + "\" does not belong to kind \"" + kind + "\"";
            }
            if ((known.required & bit) != 0 && (given & bit) == 0) {
                return where + ": kind \"" + kind + "\" needs the key \"" + key + "\"";
            }
        }
    }
    return {};
}

// Reads one property description. Returns the problem, or an empty string.
std::string ReadProperty(uint32_t type, size_t index, const Variant& item, tessera::ext::PropertyDescription& out) {
    const std::string type_part = "type " + std::to_string(type) + ", property ";
    if (item.get_type() != Variant::DICTIONARY) {
        return type_part + std::to_string(index) + ": must be a Dictionary";
    }
    const Dictionary entry = item;
    std::string where = type_part + std::to_string(index);
    if (entry.has("name") && IsText(entry["name"])) {
        where = type_part + "\"" + ToStd(String(entry["name"])) + "\"";
    }
    bool has_name = false;
    bool has_kind = false;
    unsigned given = 0;
    const Array keys = entry.keys();
    for (int64_t i = 0; i < keys.size(); ++i) {
        const Variant key_variant = keys[i];
        if (!IsText(key_variant)) {
            return where + ": the keys must be Strings";
        }
        const std::string key = ToStd(String(key_variant));
        const Variant value = entry[key_variant];
        auto wrong_type = [&](const char* wanted) {
            return where + ": key \"" + key + "\" must be " + wanted;
        };
        auto small_int = [&](uint32_t& target) -> std::string {
            if (value.get_type() != Variant::INT) {
                return wrong_type("an int");
            }
            const int64_t number = value;
            if (number < 0 || number > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
                return where + ": key \"" + key + "\" is out of range";
            }
            target = static_cast<uint32_t>(number);
            return {};
        };
        std::string problem;
        if (key == "name") {
            if (!IsText(value)) {
                return wrong_type("a String");
            }
            out.name = ToStd(String(value));
            has_name = true;
        } else if (key == "kind") {
            if (!IsText(value)) {
                return wrong_type("a String");
            }
            out.kind = ToStd(String(value));
            has_kind = true;
        } else if (key == "step" || key == "range") {
            if (!IsNumber(value)) {
                return wrong_type("a float");
            }
            (key == "step" ? out.step : out.range) = static_cast<double>(value);
            given |= key == "step" ? kStep : kRange;
        } else if (key == "bits") {
            problem = small_int(out.bits);
            given |= kBits;
        } else if (key == "count") {
            problem = small_int(out.count);
            given |= kCount;
        } else if (key == "min" || key == "max") {
            if (value.get_type() != Variant::INT) {
                return wrong_type("an int");
            }
            (key == "min" ? out.min_value : out.max_value) = static_cast<int64_t>(value);
            given |= key == "min" ? kMin : kMax;
        } else if (key == "signed") {
            if (value.get_type() != Variant::BOOL) {
                return wrong_type("a bool");
            }
            out.is_signed = static_cast<bool>(value);
            given |= kSigned;
        } else {
            return where + ": unknown key \"" + key + "\"";
        }
        if (!problem.empty()) {
            return problem;
        }
    }
    if (!has_name) {
        return where + ": missing \"name\"";
    }
    if (!has_kind) {
        return where + ": missing \"kind\"";
    }
    return CheckKeys(where, out.kind, given);
}

} // namespace

int TesseraSchema::add_type(int type_id, const Array& properties) {
    std::string problem;
    try {
        if (type_id < 0 || type_id > 255) {
            problem = "type " + std::to_string(type_id) + ": the type number must be from 0 to 255";
        } else {
            tessera::ext::TypeDescription type;
            type.type = static_cast<uint32_t>(type_id);
            for (int64_t i = 0; i < properties.size() && problem.empty(); ++i) {
                tessera::ext::PropertyDescription property;
                problem = ReadProperty(type.type, static_cast<size_t>(i), properties[i], property);
                type.properties.push_back(std::move(property));
            }
            if (problem.empty()) {
                problem = tessera::ext::ValidateType(type);
            }
            if (problem.empty() && std::any_of(types_.begin(), types_.end(), [&](const auto& known) {
                    return known.type == type.type;
                })) {
                problem = "type " + std::to_string(type.type) + " is already described";
            }
            if (problem.empty()) {
                types_.push_back(std::move(type));
                Dictionary entry;
                entry["type"] = type_id;
                entry["properties"] = properties.duplicate(true);
                described_.push_back(entry);
                error_.clear();
                return OK;
            }
        }
    } catch (const std::exception& exception) {
        problem = std::string("could not describe the type: ") + exception.what();
    }
    error_ = problem;
    UtilityFunctions::push_error(ToGodot(problem));
    return ERR_INVALID_PARAMETER;
}

std::shared_ptr<const tessera::ext::BuiltSchema> TesseraSchema::Build(std::string& error) const {
    try {
        tessera::ext::BuildResult result = tessera::ext::BuildSchema(types_);
        if (!result.schema) {
            error = result.error;
            return nullptr;
        }
        return std::shared_ptr<const tessera::ext::BuiltSchema>(std::move(result.schema));
    } catch (const std::exception& exception) {
        error = std::string("could not build the schema: ") + exception.what();
        return nullptr;
    }
}

bool TesseraSchema::is_valid() const {
    std::string error;
    return Build(error) != nullptr;
}

String TesseraSchema::get_error() const {
    return ToGodot(error_);
}

int64_t TesseraSchema::get_hash() const {
    std::string error;
    const auto built = Build(error);
    return built ? static_cast<int64_t>(built->Hash()) : 0;
}

void TesseraSchema::set_types(const Array& types) {
    types_.clear();
    described_.clear();
    error_.clear();
    for (int64_t i = 0; i < types.size(); ++i) {
        const Variant item = types[i];
        if (item.get_type() != Variant::DICTIONARY) {
            error_ = "types: item " + std::to_string(i) + " must be a Dictionary";
            UtilityFunctions::push_error(ToGodot(error_));
            continue;
        }
        const Dictionary entry = item;
        if (!entry.has("type") || entry["type"].get_type() != Variant::INT || !entry.has("properties") ||
            entry["properties"].get_type() != Variant::ARRAY) {
            error_ = "types: item " + std::to_string(i) + " needs an int \"type\" and an Array \"properties\"";
            UtilityFunctions::push_error(ToGodot(error_));
            continue;
        }
        add_type(static_cast<int>(static_cast<int64_t>(entry["type"])), entry["properties"]);
    }
}

Array TesseraSchema::get_types() const {
    return described_.duplicate(true);
}

void TesseraSchema::_bind_methods() {
    ClassDB::bind_method(D_METHOD("add_type", "type_id", "properties"), &TesseraSchema::add_type);
    ClassDB::bind_method(D_METHOD("is_valid"), &TesseraSchema::is_valid);
    ClassDB::bind_method(D_METHOD("get_error"), &TesseraSchema::get_error);
    ClassDB::bind_method(D_METHOD("get_hash"), &TesseraSchema::get_hash);
    ClassDB::bind_method(D_METHOD("set_types", "types"), &TesseraSchema::set_types);
    ClassDB::bind_method(D_METHOD("get_types"), &TesseraSchema::get_types);
    // Kept in files and in copies, not shown in the inspector: add_type is how a script fills it.
    ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "types", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_types",
                 "get_types");
}

} // namespace godot
