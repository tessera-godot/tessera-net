# Tests of the extension's logic, which needs no Godot. Added from extension/CMakeLists.txt.
file(GLOB EXTENSION_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
# server_test_guard.cpp makes a test that dies (an uncaught exception, a failed assertion) end with a message on the
# console instead of a dialog on MSVC.
add_executable(tessera_extension_tests ${EXTENSION_TEST_SOURCES}
               ${PROJECT_SOURCE_DIR}/tests/server/server_test_guard.cpp ${PROJECT_SOURCE_DIR}/tests/test_main.cpp)
target_link_libraries(tessera_extension_tests PRIVATE tessera_extension_logic tessera_server)
target_include_directories(tessera_extension_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests
                                                           ${PROJECT_SOURCE_DIR}/server/example)

if(MSVC)
    target_compile_options(tessera_extension_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_extension_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME tessera_extension_tests COMMAND tessera_extension_tests)

# The shared library carries ENet inside it, and Godot has its own: nothing but the entry point may be visible
# from outside. The script reads the export table (the `nm` output on Linux, the PE file on Windows).
find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_FOUND)
    message(FATAL_ERROR "TESSERA_BUILD_EXTENSION=ON needs a Python 3 interpreter to check what the library "
                        "exports (extension/tests/check_exports.py). Install Python 3, or leave "
                        "TESSERA_BUILD_EXTENSION off.")
endif()
add_test(NAME tessera_extension_exports
         COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/check_exports.py
                 $<TARGET_FILE:tessera_net_godot> tessera_net_init)

# The end-to-end test: a headless Godot loads the extension and talks to the example server over ENet. It needs a
# Godot executable that can run without a window; none is downloaded, so it is registered only when you give
# one: -DTESSERA_EXTENSION_GODOT=<path to the executable>.
set(TESSERA_EXTENSION_GODOT "" CACHE FILEPATH "Godot executable for the end-to-end test of the extension (optional)")
if(TESSERA_EXTENSION_GODOT AND EXISTS "${TESSERA_EXTENSION_GODOT}")
    add_test(NAME tessera_extension_godot
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/run_godot_e2e.py e2e
                     "${TESSERA_EXTENSION_GODOT}" "${CMAKE_BINARY_DIR}/extension/stage/e2e"
                     $<TARGET_FILE:tessera_example_server>)
    set_tests_properties(tessera_extension_godot PROPERTIES TIMEOUT 120)
else()
    message(STATUS "Godot executable not set: the end-to-end test is not registered")
endif()
