// SPDX-License-Identifier: Apache-2.0
#include "tessera/schema/schema_check.hpp"

#include "tessera/schema/columns.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace tessera::schema {

namespace {

// A double that does not fit a float becomes an infinity, which the replication code refuses. (A plain cast
// of such a value is not defined.)
float ToFloat(double value) {
    constexpr double kLargest = std::numeric_limits<float>::max();
    if (value > kLargest) {
        return std::numeric_limits<float>::infinity();
    }
    if (value < -kLargest) {
        return -std::numeric_limits<float>::infinity();
    }
    return static_cast<float>(value);
}

std::string Quoted(const std::string& text) {
    return "\"" + text + "\"";
}

// What to change when a property of this kind is refused by itself. The limits are the ones of the
// replication code.
std::string FixForProperty(const PropertyDecl& p) {
    switch (p.kind) {
    case Kind::Position:
        return "\"step\" and \"range\" are numbers above 0, and 2 x \"range\" / \"step\" must be at most "
               "2147483647";
    case Kind::Angle:
        return "\"bits\" is a whole number from 1 to 16";
    case Kind::Integer:
        return "\"min\" must not be above \"max\", both must fit the column, and with \"signed\" false "
               "\"min\" cannot be "
               "below 0";
    case Kind::Mask:
        return "\"bits\" is a whole number from 1 to 64";
    case Kind::Choice:
        return "\"count\" is a whole number from 2";
    case Kind::Flag:
        break;
    }
    return {};
}

std::vector<net::PropertyDescriptor> Descriptors(const TypeDecl& type, size_t count) {
    std::vector<net::PropertyDescriptor> descriptors;
    descriptors.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        descriptors.push_back(MakeDescriptor(type.properties[i]));
    }
    return descriptors;
}

// The place and the correction for a type the replication code refused. It is found without reading the
// message: the first number of properties that is refused points at the property that made it so.
Diagnostic Explain(const TypeDecl& type, const std::string& core_message) {
    const std::string prefix = "type " + Quoted(type.name) + " (" + std::to_string(type.type) + "): ";
    Diagnostic diagnostic{type.where.position, prefix + core_message, {}};
    for (size_t k = 1; k <= type.properties.size(); ++k) {
        net::ReplicationSchema probe;
        if (probe.AddType(type.type, Descriptors(type, k))) {
            continue;
        }
        const PropertyDecl& culprit = type.properties[k - 1];
        diagnostic.position = culprit.where.position;
        net::ReplicationSchema alone;
        if (!alone.AddType(type.type, {MakeDescriptor(culprit)})) {
            diagnostic.fix = FixForProperty(culprit);
        } else if (k > net::ReplicationSchema::kMaxPropertiesPerType) {
            diagnostic.fix = "a type has at most " +
                             std::to_string(net::ReplicationSchema::kMaxPropertiesPerType) +
                             " properties; remove this one";
        } else {
            diagnostic.fix =
                "the replicated state of a type takes at most " +
                std::to_string(net::ReplicationSchema::kMaxWordsPerEntity) +
                " words (a position takes 3, an integer or a mask 2, an angle or a choice 1); remove "
                "or shrink a property";
        }
        break;
    }
    if (type.properties.empty()) {
        diagnostic.fix = "add at least one property to the type";
    }
    return diagnostic;
}

} // namespace

net::PropertyDescriptor MakeDescriptor(const PropertyDecl& property) {
    const ColumnSpec column = ColumnFor(property);
    const net::ColumnRef ref{0, column.bytes};
    switch (property.kind) {
    case Kind::Position:
        return net::PropertyDescriptor::Position(ref, ref, ref, ToFloat(property.step),
                                                 ToFloat(property.range));
    case Kind::Angle:
        return net::PropertyDescriptor::Angle(ref, property.bits);
    case Kind::Integer:
        return net::PropertyDescriptor::Integer(ref, property.is_signed, property.min_value,
                                                property.max_value);
    case Kind::Flag:
        return net::PropertyDescriptor::Bits(ref, 1);
    case Kind::Mask:
        return net::PropertyDescriptor::Bits(ref, property.bits);
    case Kind::Choice:
        return net::PropertyDescriptor::Enum(ref, property.count);
    }
    return {};
}

CheckResult Check(const SchemaFile& file) {
    CheckResult result;
    auto schema = std::make_unique<net::ReplicationSchema>();
    std::vector<const TypeDecl*> types;
    types.reserve(file.types.size());
    for (const TypeDecl& type : file.types) {
        types.push_back(&type);
    }
    std::stable_sort(types.begin(), types.end(),
                     [](const TypeDecl* a, const TypeDecl* b) { return a->type < b->type; });
    for (const TypeDecl* type : types) {
        std::string message;
        if (!schema->AddType(type->type, Descriptors(*type, type->properties.size()), &message)) {
            result.diagnostics.push_back(Explain(*type, message));
        }
    }
    std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(),
                     [](const Diagnostic& a, const Diagnostic& b) {
                         return a.position.line != b.position.line ? a.position.line < b.position.line
                                                                   : a.position.column < b.position.column;
                     });
    if (result.diagnostics.empty()) {
        result.hash = schema->Hash();
        result.schema = std::move(schema);
    }
    return result;
}

} // namespace tessera::schema
