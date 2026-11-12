// SPDX-License-Identifier: Apache-2.0
// The GDCLASS macro of godot-cpp declares a local variable that the compiler finds unused in classes that do not
// override the notification hook (MSVC warning C4189, clang -Wunused-variable). The wrappers below silence exactly
// that, around the class declaration and nowhere else, so that the rest of our code keeps the full warning level.
#pragma once

#if defined(__clang__)
#define TESSERA_GODOT_CLASS_BEGIN                                                                            \
    _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wunused-variable\"")
#define TESSERA_GODOT_CLASS_END _Pragma("clang diagnostic pop")
#elif defined(_MSC_VER)
#define TESSERA_GODOT_CLASS_BEGIN __pragma(warning(push)) __pragma(warning(disable : 4189))
#define TESSERA_GODOT_CLASS_END __pragma(warning(pop))
#else
#define TESSERA_GODOT_CLASS_BEGIN
#define TESSERA_GODOT_CLASS_END
#endif
