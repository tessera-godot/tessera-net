// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "godot_warnings.hpp"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// The client of a Tessera server, as seen from GDScript. It is filled in step by step; for now it only
// answers, so that the build and the loading can be checked on their own.
TESSERA_GODOT_CLASS_BEGIN
class TesseraClient : public RefCounted {
    GDCLASS(TesseraClient, RefCounted)

public:
    String ping() const;

protected:
    static void _bind_methods();
};
TESSERA_GODOT_CLASS_END

} // namespace godot
