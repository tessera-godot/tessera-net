# SPDX-License-Identifier: Apache-2.0
# tessera_schema: reads a schema file, checks it with the same code the server uses, and writes C++ from it.
# Built with -DTESSERA_BUILD_SCHEMA_TOOL=ON, and always with -DTESSERA_BUILD_SERVER=ON (see the top-level
# CMakeLists.txt). The library depends on the core library and nothing else.
add_library(tessera_schema_tool STATIC
    src/cli.cpp
    src/json_reader.cpp
    src/schema_file.cpp
    src/names.cpp
    src/godot_names.cpp
    src/columns.cpp
    src/schema_check.cpp
    src/generator_text.cpp
    src/cpp_generator.cpp)

target_include_directories(tessera_schema_tool PUBLIC include)
target_link_libraries(tessera_schema_tool PUBLIC tessera_net_core)

add_executable(tessera_schema tool/main.cpp)
target_link_libraries(tessera_schema PRIVATE tessera_schema_tool)

if(MSVC)
    target_compile_options(tessera_schema_tool PRIVATE /W4 /permissive-)
    target_compile_options(tessera_schema PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_schema_tool PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(tessera_schema PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Makes a header from a schema file while the project is built, and an INTERFACE library that carries its folder, so
# that a target which links the library can include the header, and is built after it. Used inside this repository
# for now: the example server and the tests.
#   tessera_schema_generate_cpp(<target> SCHEMA <schema.json> OUTPUT <header path> NAMESPACE <a::b>)
function(tessera_schema_generate_cpp target)
    cmake_parse_arguments(ARG "" "SCHEMA;OUTPUT;NAMESPACE" "" ${ARGN})
    if(NOT ARG_SCHEMA OR NOT ARG_OUTPUT OR NOT ARG_NAMESPACE)
        message(FATAL_ERROR "tessera_schema_generate_cpp needs SCHEMA, OUTPUT and NAMESPACE")
    endif()
    get_filename_component(output_dir "${ARG_OUTPUT}" DIRECTORY)
    add_custom_command(
        OUTPUT "${ARG_OUTPUT}"
        COMMAND tessera_schema generate --cpp "${ARG_OUTPUT}" --namespace "${ARG_NAMESPACE}" "${ARG_SCHEMA}"
        DEPENDS tessera_schema "${ARG_SCHEMA}"
        COMMENT "Generating ${ARG_OUTPUT} from ${ARG_SCHEMA}"
        VERBATIM)
    add_custom_target(${target}_generate DEPENDS "${ARG_OUTPUT}")
    add_library(${target} INTERFACE)
    target_include_directories(${target} INTERFACE "${output_dir}")
    add_dependencies(${target} ${target}_generate)
endfunction()
