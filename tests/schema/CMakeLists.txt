# SPDX-License-Identifier: Apache-2.0
# Tests of the schema tool. Built when the tool is (see tests/CMakeLists.txt). They have their own executable, so that
# the main test program is the same with or without the tool.
file(GLOB SCHEMA_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
# schema_test_guard.cpp makes a test that dies end with a message on the console instead of a dialog on MSVC.
add_executable(tessera_schema_tests ${SCHEMA_TEST_SOURCES} schema_test_guard.cpp
               ${PROJECT_SOURCE_DIR}/tests/test_main.cpp)
# The header that the tool writes from a schema file of the tests, included by two of the test files.
tessera_schema_generate(tessera_all_kinds_schema
                        SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/all_kinds.json
                        CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/all_kinds.hpp
                        NAMESPACE all_kinds)

tessera_schema_generate(tessera_risky_names_schema
                        SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/risky_names.json
                        CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/risky_names.hpp
                        NAMESPACE risky::tessera)

target_link_libraries(tessera_schema_tests PRIVATE tessera_schema_tool tessera_all_kinds_schema tessera_risky_names_schema)
target_include_directories(tessera_schema_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(tessera_schema_tests PRIVATE TESSERA_SCHEMA_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                           TESSERA_EXAMPLE_SCHEMA_FILE="${PROJECT_SOURCE_DIR}/server/example/example_schema.json"
                           TESSERA_REPOSITORY_DIR="${PROJECT_SOURCE_DIR}")

if(MSVC)
    target_compile_options(tessera_schema_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_schema_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME tessera_schema_tests COMMAND tessera_schema_tests)

# The real tool, run as a process twice, with the outputs compared.
add_test(NAME tessera_schema_cli_stable
         COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:tessera_schema>
                 -DSCHEMA=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/all_kinds.json
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/stable/$<CONFIG> -P ${CMAKE_CURRENT_SOURCE_DIR}/check_stable.cmake)

# The schema file of the example server is a good one, and the tool says so.
add_test(NAME tessera_schema_example_check
         COMMAND tessera_schema check ${PROJECT_SOURCE_DIR}/server/example/example_schema.json)
set_tests_properties(tessera_schema_example_check PROPERTIES PASS_REGULAR_EXPRESSION "OK, 2 types, hash 0x04b6ebf2e86202d0")

# The script of the Godot demo is kept in the repository, and has to be what the tool makes from the schema file that
# the example server uses (the demo and the server describe the same entities).
tessera_schema_check_test(tessera_schema_demo_check
                          SCHEMA ${PROJECT_SOURCE_DIR}/server/example/example_schema.json
                          GDSCRIPT ${PROJECT_SOURCE_DIR}/examples/godot-client-demo/example_schema.gd
                          CLASS ExampleSchema)

# The functions of the schema tool (schema/cmake/tessera_schema.cmake) stop with a message that says what is wrong
# when they are called the wrong way.
add_test(NAME tessera_schema_function_usage
         COMMAND ${CMAKE_COMMAND} -DFUNCTIONS=${PROJECT_SOURCE_DIR}/schema/cmake/tessera_schema.cmake
                 -DSCHEMA=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/all_kinds.json
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/function_usage/$<CONFIG>
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_function_usage.cmake)

# The example project (examples/game-schema), copied and built as a project of its own that brings this repository in
# as a folder: what a game does. It configures and builds the core library and the tool a second time, which takes
# about a minute, so it is off unless asked for.
option(TESSERA_TEST_SCHEMA_PROJECT "Also build the schema example as a separate project and test it (about a minute)" OFF)
if(TESSERA_TEST_SCHEMA_PROJECT)
    add_test(NAME tessera_schema_in_a_project
             COMMAND ${CMAKE_COMMAND} -DREPO=${PROJECT_SOURCE_DIR} -DEXAMPLE=${PROJECT_SOURCE_DIR}/examples/game-schema
                     -DTOOL=$<TARGET_FILE:tessera_schema> -DWORK=${CMAKE_CURRENT_BINARY_DIR}/project/$<CONFIG>
                     -DGENERATOR=${CMAKE_GENERATOR} -DPLATFORM=${CMAKE_GENERATOR_PLATFORM}
                     -DCXX=${CMAKE_CXX_COMPILER} -DCONFIG=$<CONFIG>
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/check_project.cmake)
    set_tests_properties(tessera_schema_in_a_project PROPERTIES TIMEOUT 900)
endif()
