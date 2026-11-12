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
        } else if (key == "bits") {
            problem = small_int(out.bits);
        } else if (key == "count") {
            problem = small_int(out.count);
        } else if (key == "min" || key == "max") {
            if (value.get_type() != Variant::INT) {
                return wrong_type("an int");
            }
            (key == "min" ? out.min_value : out.max_value) = static_cast<int64_t>(value);
        } else if (key == "signed") {
            if (value.get_type() != Variant::BOOL) {
                return wrong_type("a bool");
            }
            out.is_signed = static_cast<bool>(value);
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
    return {};
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

void TesseraSchema::_bind_methods() {
    ClassDB::bind_method(D_METHOD("add_type", "type_id", "properties"), &TesseraSchema::add_type);
    ClassDB::bind_method(D_METHOD("is_valid"), &TesseraSchema::is_valid);
    ClassDB::bind_method(D_METHOD("get_error"), &TesseraSchema::get_error);
    ClassDB::bind_method(D_METHOD("get_hash"), &TesseraSchema::get_hash);
}

} // namespace godot
