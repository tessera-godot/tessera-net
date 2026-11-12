# The Godot extension. Built only with -DTESSERA_BUILD_EXTENSION=ON (see the top-level CMakeLists.txt); the first
# configure downloads godot-cpp, pinned to one commit.
include(FetchContent)

# godot-cpp compiles for one target at a time (GODOTCPP_TARGET), so a build is either a debug or a release one.
# These options are what lets its code sit next to ours: our core throws exceptions and links the shared C
# runtime, so godot-cpp must not turn the first off or ask for the static runtime.
set(GODOTCPP_DISABLE_EXCEPTIONS OFF CACHE BOOL "" FORCE)
set(GODOTCPP_USE_STATIC_CPP OFF CACHE BOOL "" FORCE)
set(GODOTCPP_SYSTEM_HEADERS ON CACHE BOOL "" FORCE)
set(GODOTCPP_SYMBOL_VISIBILITY hidden CACHE STRING "" FORCE)

FetchContent_Declare(godot_cpp
    GIT_REPOSITORY https://github.com/godotengine/godot-cpp.git
    # The commit of the 4.5 branch that the extension is built against. A pinned commit, so that what is fetched
    # does not depend on a branch staying where it is. To use a copy you already have, pass
    # -DFETCHCONTENT_SOURCE_DIR_GODOT_CPP=<path>.
    GIT_TAG 27d9dd23c83871e0619fca5dc2cddfbfd69e926a)
FetchContent_MakeAvailable(godot_cpp)

# Everything that does not need Godot: describing a schema, converting values, the client session. Tested on its
# own, with the simulated network (extension/tests).
add_library(tessera_extension_logic STATIC
    logic/src/schema_description.cpp
    logic/src/value_conversion.cpp
    logic/src/client_session.cpp)
target_include_directories(tessera_extension_logic PUBLIC logic/include)
target_link_libraries(tessera_extension_logic PUBLIC tessera_net_core)
if(MSVC)
    target_compile_options(tessera_extension_logic PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_extension_logic PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(TESSERA_BUILD_TESTS)
    add_subdirectory(tests)
endif()

add_library(tessera_net_godot SHARED
    src/register_types.cpp
    src/tessera_client.cpp
    src/tessera_schema.cpp)

target_link_libraries(tessera_net_godot PRIVATE godot::cpp tessera_extension_logic tessera_net_enet)
target_compile_features(tessera_net_godot PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(tessera_net_godot PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_net_godot PRIVATE -Wall -Wextra -Wpedantic)
    # Nothing from the static libraries linked in may be visible from outside (ENet's functions in particular), and
    # nothing but the entry point at all: the version script lists the one name that stays.
    target_link_options(tessera_net_godot PRIVATE -Wl,--exclude-libs,ALL
                        -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/tessera_net.map)
    set_target_properties(tessera_net_godot PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tessera_net.map)
endif()

# The file name Godot expects: libtessera_net.<platform>.<target>.<arch>.<dll or so>. Only x86_64 on Windows and
# Linux is set up.
if(WIN32)
    set(TESSERA_EXTENSION_PLATFORM windows)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(TESSERA_EXTENSION_PLATFORM linux)
else()
    message(FATAL_ERROR "The Godot extension is set up for Windows and Linux only.")
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|X86_64)$")
    message(FATAL_ERROR "The Godot extension is set up for x86_64 only (this is ${CMAKE_SYSTEM_PROCESSOR}).")
endif()
set(TESSERA_EXTENSION_SUFFIX ".${TESSERA_EXTENSION_PLATFORM}.${GODOTCPP_TARGET}.x86_64")
set_target_properties(tessera_net_godot PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "tessera_net${TESSERA_EXTENSION_SUFFIX}"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/extension/lib>"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/extension/lib>")

configure_file(tessera_net.gdextension.in "${CMAKE_BINARY_DIR}/extension/tessera_net.gdextension" COPYONLY)

# A small Godot project with the extension in it, put together in the build directory (never in the source tree):
# what the end-to-end test runs.
add_custom_target(tessera_extension_stage ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/extension/stage/e2e/bin"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/tests/godot"
            "${CMAKE_BINARY_DIR}/extension/stage/e2e"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/extension/tessera_net.gdextension"
            "${CMAKE_BINARY_DIR}/extension/stage/e2e/tessera_net.gdextension"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:tessera_net_godot>"
            "${CMAKE_BINARY_DIR}/extension/stage/e2e/bin/"
    DEPENDS tessera_net_godot
    COMMENT "Putting together the test project for the extension")

# The demo project with the library next to it, put together in the build directory: open
# build/<preset>/extension/stage/demo in Godot.
add_custom_target(tessera_extension_stage_demo
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/extension/stage/demo/bin"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/examples/godot-client-demo"
            "${CMAKE_BINARY_DIR}/extension/stage/demo"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/extension/tessera_net.gdextension"
            "${CMAKE_BINARY_DIR}/extension/stage/demo/tessera_net.gdextension"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:tessera_net_godot>"
            "${CMAKE_BINARY_DIR}/extension/stage/demo/bin/"
    DEPENDS tessera_net_godot
    COMMENT "Putting together the demo project")
