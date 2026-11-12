// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "godot_warnings.hpp"
#include "tessera/ext/schema_description.hpp"

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <memory>
#include <string>
#include <vector>

namespace godot {

// The properties that the server replicates for each type of entity, described from GDScript. The client has
// to describe them exactly as the server does: the same types, the same properties in the same order, and the
// same kind and numbers for each one (bits, step and range, min, max, signed, count). The names are the client's own.
TESSERA_GODOT_CLASS_BEGIN
class TesseraSchema : public Resource {
    GDCLASS(TesseraSchema, Resource)

public:
    // Adds a type. Each property is a Dictionary with "name" and "kind" and the keys its kind uses. Returns OK, or
    // ERR_INVALID_PARAMETER with the reason in get_error() (and in the editor's output), changing nothing.
    int add_type(int type_id, const Array& properties);
    bool is_valid() const;
    String get_error() const;
    int64_t get_hash() const;

    // What was described, as an Array with one Dictionary ({"type", "properties"}) for each type. Setting it replaces
    // everything. This is what lets a schema be saved in a .tres file and copied with duplicate().
    void set_types(const Array& types);
    Array get_types() const;

    // For the client: the schema built from what was described, or null with the reason in `error`.
    std::shared_ptr<const tessera::ext::BuiltSchema> Build(std::string& error) const;

protected:
    static void _bind_methods();

private:
    std::vector<tessera::ext::TypeDescription> types_;
    Array described_; // the same types as they were given, for get_types()
    std::string error_;
};
TESSERA_GODOT_CLASS_END

} // namespace godot
