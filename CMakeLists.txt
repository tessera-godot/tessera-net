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
option(TESSERA_BUILD_EXTENSION "Build the Godot extension (downloads godot-cpp; needs ENet and the server)" OFF)

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

# The core library has no dependency on Godot (see docs/adr/0001-engine-independent-core.md).
add_subdirectory(core)

if(TESSERA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
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
