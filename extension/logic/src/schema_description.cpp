// SPDX-License-Identifier: Apache-2.0
#include "tessera/ext/schema_description.hpp"

#include "tessera/net/entity_id.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace tessera::ext {

namespace {

constexpr double kMaxPositionSteps = 2147483647.0; // 2^31 - 1, what the core allows
constexpr const char* kKnownKinds = "position, angle, integer, flag, mask, choice";

// The start of every message about one property: `type 1, property "hp"`.
std::string Where(uint32_t type, size_t index, const PropertyDescription& property) {
    std::string where = "type " + std::to_string(type) + ", property ";
    if (property.name.empty()) {
        return where + std::to_string(index);
    }
    return where + "\"" + property.name + "\"";
}

bool KindOf(const std::string& text, ValueKind& kind) {
    static const std::pair<const char*, ValueKind> kinds[] = {
        {"position", ValueKind::Position}, {"angle", ValueKind::Angle}, {"integer", ValueKind::Integer},
        {"flag", ValueKind::Flag},         {"mask", ValueKind::Mask},   {"choice", ValueKind::Choice}};
    for (const auto& [name, value] : kinds) {
        if (text == name) {
            kind = value;
            return true;
        }
    }
    return false;
}

// The client never reads a column, so the ones in its descriptors only have to exist and be wide enough.
net::ColumnRef Column(uint8_t bytes) {
    return net::ColumnRef{0, bytes};
}

uint8_t IntegerColumnBytes(const PropertyDescription& p) {
    if (p.is_signed) {
        if (p.min_value >= -128 && p.max_value <= 127) {
            return 1;
        }
        if (p.min_value >= -32768 && p.max_value <= 32767) {
            return 2;
        }
        if (p.min_value >= std::numeric_limits<int32_t>::min() &&
            p.max_value <= std::numeric_limits<int32_t>::max()) {
            return 4;
        }
        return 8;
    }
    if (p.max_value <= 255) {
        return 1;
    }
    if (p.max_value <= 65535) {
        return 2;
    }
    return p.max_value <= static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) ? 4 : 8;
}

uint8_t MaskColumnBytes(uint32_t bits) {
    return bits <= 8 ? 1 : bits <= 16 ? 2 : bits <= 32 ? 4 : 8;
}

uint8_t ChoiceColumnBytes(uint32_t count) {
    const uint32_t largest = count - 1;
    return largest <= 255 ? 1 : largest <= 65535 ? 2 : 4;
}

// Checks one property. Returns the problem, or an empty string, and the descriptor when there is none.
std::string DescribeProperty(uint32_t type, size_t index, const PropertyDescription& p,
                             net::PropertyDescriptor& out, ValueKind& kind) {
    const std::string where = Where(type, index, p);
    if (!KindOf(p.kind, kind)) {
        return where + ": unknown kind \"" + p.kind + "\" (use one of: " + kKnownKinds + ")";
    }
    switch (kind) {
    case ValueKind::Position: {
        if (!std::isfinite(p.step) || !(p.step > 0.0)) {
            return where + ": \"step\" must be a number above 0";
        }
        if (!std::isfinite(p.range) || !(p.range > 0.0)) {
            return where + ": \"range\" must be a number above 0";
        }
        const float step = static_cast<float>(p.step);
        const float range = static_cast<float>(p.range);
        if (!(step > 0.0f) || !std::isfinite(step)) {
            return where + ": \"step\" does not fit a float";
        }
        if (!std::isfinite(range)) {
            return where + ": \"range\" does not fit a float";
        }
        const double steps = std::round(2.0 * static_cast<double>(range) / static_cast<double>(step));
        if (!(steps <= kMaxPositionSteps)) {
            return where + ": \"range\" divided by \"step\" is too large (the limit is 2^31 - 1 steps)";
        }
        out = net::PropertyDescriptor::Position(Column(4), Column(4), Column(4), step, range);
        return {};
    }
    case ValueKind::Angle:
        if (p.bits < 1 || p.bits > 16) {
            return where + ": \"bits\" must be from 1 to 16";
        }
        out = net::PropertyDescriptor::Angle(Column(4), p.bits);
        return {};
    case ValueKind::Integer:
        if (p.min_value > p.max_value) {
            return where + ": \"min\" is above \"max\"";
        }
        if (!p.is_signed && p.min_value < 0) {
            return where + ": \"min\" can't be below 0 when \"signed\" is false";
        }
        out = net::PropertyDescriptor::Integer(Column(IntegerColumnBytes(p)), p.is_signed, p.min_value,
                                               p.max_value);
        return {};
    case ValueKind::Flag:
        out = net::PropertyDescriptor::Bits(Column(1), 1);
        return {};
    case ValueKind::Mask:
        if (p.bits < 1 || p.bits > 64) {
            return where + ": \"bits\" must be from 1 to 64";
        }
        out = net::PropertyDescriptor::Bits(Column(MaskColumnBytes(p.bits)), p.bits);
        return {};
    case ValueKind::Choice:
        if (p.count < 2) {
            return where + ": \"count\" must be at least 2";
        }
        out = net::PropertyDescriptor::Enum(Column(ChoiceColumnBytes(p.count)), p.count);
        return {};
    }
    return where + ": unknown kind";
}

// Checks one type and, when it is fine, makes its descriptors and infos.
std::string PrepareType(const TypeDescription& type, std::vector<net::PropertyDescriptor>& descriptors,
                        std::vector<PropertyInfo>& infos) {
    const std::string name = "type " + std::to_string(type.type);
    if (type.type > net::EntityId::kMaxType) {
        return name + ": the largest type number is " + std::to_string(net::EntityId::kMaxType);
    }
    if (type.properties.empty()) {
        return name + ": needs at least one property";
    }
    for (size_t i = 0; i < type.properties.size(); ++i) {
        const PropertyDescription& property = type.properties[i];
        if (property.name.empty()) {
            return name + ", property " + std::to_string(i) + ": the name is empty";
        }
        for (size_t earlier = 0; earlier < i; ++earlier) {
            if (type.properties[earlier].name == property.name) {
                return Where(type.type, i, property) + ": the name is used more than once";
            }
        }
        net::PropertyDescriptor descriptor;
        ValueKind kind = ValueKind::Integer;
        const std::string problem = DescribeProperty(type.type, i, property, descriptor, kind);
        if (!problem.empty()) {
            return problem;
        }
        descriptors.push_back(descriptor);
        infos.push_back(PropertyInfo{property.name, kind});
    }
    return {};
}

} // namespace

BuiltSchema::BuiltSchema(std::unique_ptr<net::ReplicationSchema> schema,
                         std::map<uint32_t, std::vector<PropertyInfo>> properties)
    : schema_(std::move(schema)), properties_(std::move(properties)) {
}

const std::vector<PropertyInfo>* BuiltSchema::Properties(uint32_t type) const noexcept {
    const auto found = properties_.find(type);
    return found == properties_.end() ? nullptr : &found->second;
}

std::string ValidateType(const TypeDescription& type) {
    std::vector<net::PropertyDescriptor> descriptors;
    std::vector<PropertyInfo> infos;
    std::string problem = PrepareType(type, descriptors, infos);
    if (!problem.empty()) {
        return problem;
    }
    // What is left to find is what only the core knows: too many properties, too many words.
    net::ReplicationSchema trial;
    std::string core_problem;
    if (!trial.AddType(type.type, std::move(descriptors), &core_problem)) {
        return "type " + std::to_string(type.type) + ": " + core_problem;
    }
    return {};
}

BuildResult BuildSchema(std::span<const TypeDescription> types) {
    if (types.empty()) {
        return BuildResult{nullptr, "the schema has no types"};
    }
    std::vector<const TypeDescription*> ordered;
    for (const TypeDescription& type : types) {
        ordered.push_back(&type);
    }
    // The core orders the types by number whatever the order they come in; so does the check for repeats.
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const TypeDescription* a, const TypeDescription* b) { return a->type < b->type; });
    for (size_t i = 1; i < ordered.size(); ++i) {
        if (ordered[i]->type == ordered[i - 1]->type) {
            return BuildResult{nullptr, "type " + std::to_string(ordered[i]->type) + " is described twice"};
        }
    }

    auto schema = std::make_unique<net::ReplicationSchema>();
    std::map<uint32_t, std::vector<PropertyInfo>> properties;
    for (const TypeDescription* type : ordered) {
        std::vector<net::PropertyDescriptor> descriptors;
        std::vector<PropertyInfo> infos;
        std::string problem = PrepareType(*type, descriptors, infos);
        if (problem.empty()) {
            std::string core_problem;
            if (!schema->AddType(type->type, std::move(descriptors), &core_problem)) {
                problem = "type " + std::to_string(type->type) + ": " + core_problem;
            }
        }
        if (!problem.empty()) {
            return BuildResult{nullptr, problem};
        }
        properties[type->type] = std::move(infos);
    }
    return BuildResult{std::make_unique<BuiltSchema>(std::move(schema), std::move(properties)), {}};
}

} // namespace tessera::ext
