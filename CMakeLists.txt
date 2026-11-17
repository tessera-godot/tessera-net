cmake_minimum_required(VERSION 3.21)

project(tessera_net
    VERSION 0.0.1
    DESCRIPTION "High-performance dedicated-server networking for Godot"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TESSERA_BUILD_TESTS "Build the unit tests" ON)
option(TESSERA_BUILD_BENCH "Build the acceptance benchmark" OFF)
option(TESSERA_PROFILE "Time the stages of the network phase (measurement builds only)" OFF)
option(TESSERA_BUILD_ECS_COMPARISON "Build the comparison with a general-purpose ECS (downloads EnTT)" OFF)
option(TESSERA_BUILD_ENET "Build the ENet transport, its tests and the head-to-head programs (downloads ENet)" OFF)
option(TESSERA_BUILD_SERVER "Build the server library, its example and its tests" OFF)
option(TESSERA_BUILD_SCHEMA_TOOL "Build tessera_schema, the tool that checks a schema file and generates code from it" OFF)
option(TESSERA_BUILD_EXTENSION "Build the Godot extension (downloads godot-cpp; needs ENet and the server)" OFF)
set(TESSERA_SANITIZE "" CACHE STRING "Sanitizers to build everything with, for example address,undefined (GCC and Clang only)")

# The extension is a shared library that carries the core and ENet inside it, so everything it links is built
# position independent and with hidden symbols (only the entry point stays visible). That is set here, before
# the targets exist, and only when the extension is asked for: the other builds stay as they are.
if(TESSERA_BUILD_EXTENSION)
    if(NOT TESSERA_BUILD_ENET OR NOT TESSERA_BUILD_SERVER)
        message(FATAL_ERROR "TESSERA_BUILD_EXTENSION=ON needs TESSERA_BUILD_ENET=ON and TESSERA_BUILD_SERVER=ON: "
                            "the extension talks ENet, and its tests and demo use the example server.")
    endif()
    if(MSVC)
        # One C runtime for everything that goes into the library, in every configuration: the shared one, which
        # needs nothing installed (godot-cpp asks for the same). Set here because the core is built before
        # godot-cpp is read; without it a Debug build links two different runtimes and fails.
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    endif()
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
    set(CMAKE_C_VISIBILITY_PRESET hidden)
    set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
endif()

# Sanitizers are off unless asked for. When on, they apply to every target made after this point (the core, the server,
# ENet, godot-cpp and the tests), so that a memory or undefined-behavior error in any of them stops the program with
# a report instead of going unnoticed. The library is not meant to be loaded into Godot in this configuration.
if(TESSERA_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "TESSERA_SANITIZE is for GCC and Clang. With MSVC, leave it empty.")
    endif()
    add_compile_options(-fsanitize=${TESSERA_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${TESSERA_SANITIZE})
endif()

# The example server gets its entity columns from a schema file, turned into C++ while it is built, so building the
# server always builds the schema tool too. It can also be built alone.
set(TESSERA_WITH_SCHEMA_TOOL OFF)
if(TESSERA_BUILD_SCHEMA_TOOL OR TESSERA_BUILD_SERVER)
    set(TESSERA_WITH_SCHEMA_TOOL ON)
    if(NOT TESSERA_BUILD_SCHEMA_TOOL)
        message(STATUS "TESSERA_BUILD_SERVER=ON: the schema tool is built too (the example server is generated from a schema file)")
    endif()
endif()

# The core library has no dependency on Godot (see docs/adr/0001-engine-independent-core.md).
add_subdirectory(core)

if(TESSERA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(TESSERA_WITH_SCHEMA_TOOL)
    add_subdirectory(schema)
endif()

if(TESSERA_BUILD_SERVER)
    add_subdirectory(server)
endif()

if(TESSERA_BUILD_EXTENSION)
    add_subdirectory(extension)
endif()

if(TESSERA_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(TESSERA_BUILD_ECS_COMPARISON)
    add_subdirectory(bench/ecs-comparison)
endif()

if(TESSERA_BUILD_ENET)
    add_subdirectory(bench/head-to-head)
endif()
