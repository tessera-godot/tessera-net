# Tests of the server library. Built only with -DTESSERA_BUILD_SERVER=ON (see tests/CMakeLists.txt).
file(GLOB SERVER_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_server_*.cpp)
add_executable(tessera_server_tests ${SERVER_TEST_SOURCES} server_test_guard.cpp
               ${PROJECT_SOURCE_DIR}/tests/test_main.cpp)
target_link_libraries(tessera_server_tests PRIVATE tessera_server)
target_include_directories(tessera_server_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests
                                                       ${PROJECT_SOURCE_DIR}/server/example)

if(MSVC)
    target_compile_options(tessera_server_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_server_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME tessera_server_tests COMMAND tessera_server_tests)

# The example server over real sockets, and as a process that gets signals. Only with ENet.
if(TESSERA_BUILD_ENET)
    add_executable(tessera_server_enet_tests test_enet_server_example.cpp test_enet_stop_signals.cpp
                   ${PROJECT_SOURCE_DIR}/server/example/stop_signals.cpp server_test_guard.cpp
                   ${PROJECT_SOURCE_DIR}/tests/test_main.cpp)
    target_link_libraries(tessera_server_enet_tests PRIVATE tessera_server tessera_net_enet)
    target_include_directories(tessera_server_enet_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests
                                                                 ${PROJECT_SOURCE_DIR}/server/example)
    if(MSVC)
        target_compile_options(tessera_server_enet_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(tessera_server_enet_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME tessera_server_enet_tests COMMAND tessera_server_enet_tests)

    # The test that sends real signals to the example server is a Python script. A build that has ENet on but
    # cannot run it must say so, not quietly drop the test.
    find_package(Python3 COMPONENTS Interpreter)
    if(NOT Python3_FOUND)
        message(FATAL_ERROR "TESSERA_BUILD_ENET=ON needs a Python 3 interpreter to run the example server "
                            "process test (tests/server/run_example_process.py). Install Python 3, or leave "
                            "TESSERA_BUILD_ENET off.")
    endif()
    add_test(NAME tessera_example_server_process
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/run_example_process.py
                     $<TARGET_FILE:tessera_example_server>)
    set_tests_properties(tessera_example_server_process PROPERTIES TIMEOUT 120)
endif()
