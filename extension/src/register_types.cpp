// SPDX-License-Identifier: Apache-2.0
#include "tessera_client.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

namespace {

// Nothing here keeps state: every class registers itself and nothing is left behind when Godot unloads us.
void InitializeTesseraNet(ModuleInitializationLevel level) {
    if (level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
    GDREGISTER_CLASS(TesseraClient);
}

void UninitializeTesseraNet(ModuleInitializationLevel) {
}

} // namespace

extern "C" {
GDExtensionBool GDE_EXPORT tessera_net_init(GDExtensionInterfaceGetProcAddress get_proc_address,
                                            GDExtensionClassLibraryPtr library,
                                            GDExtensionInitialization* initialization) {
    GDExtensionBinding::InitObject init_object(get_proc_address, library, initialization);
    init_object.register_initializer(InitializeTesseraNet);
    init_object.register_terminator(UninitializeTesseraNet);
    init_object.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init_object.init();
}
}
