# SPDX-License-Identifier: Apache-2.0
# Tests of the schema tool. Built when the tool is (see tests/CMakeLists.txt). They have their own executable, so that
# the main test program is the same with or without the tool.
file(GLOB SCHEMA_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
# schema_test_guard.cpp makes a test that dies end with a message on the console instead of a dialog on MSVC.
add_executable(tessera_schema_tests ${SCHEMA_TEST_SOURCES} schema_test_guard.cpp
               ${PROJECT_SOURCE_DIR}/tests/test_main.cpp)
# The header that the tool writes from a schema file of the tests, included by two of the test files.
tessera_schema_generate_cpp(tessera_all_kinds_schema
                            SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/all_kinds.json
                            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/all_kinds.hpp
                            NAMESPACE all_kinds)

tessera_schema_generate_cpp(tessera_risky_names_schema
                            SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/risky_names.json
                            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/risky_names.hpp
                            NAMESPACE risky)

target_link_libraries(tessera_schema_tests PRIVATE tessera_schema_tool tessera_all_kinds_schema tessera_risky_names_schema)
target_include_directories(tessera_schema_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(tessera_schema_tests PRIVATE TESSERA_SCHEMA_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

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
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/stable -P ${CMAKE_CURRENT_SOURCE_DIR}/check_stable.cmake)

# The schema file of the example server is a good one, and the tool says so.
add_test(NAME tessera_schema_example_check
         COMMAND tessera_schema check ${PROJECT_SOURCE_DIR}/server/example/example_schema.json)
set_tests_properties(tessera_schema_example_check PROPERTIES PASS_REGULAR_EXPRESSION "OK, 2 types, hash 0x04b6ebf2e86202d0")
