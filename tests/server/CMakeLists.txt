# Tests of the server library. Built only with -DTESSERA_BUILD_SERVER=ON (see tests/CMakeLists.txt).
file(GLOB SERVER_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_server_*.cpp)
add_executable(tessera_server_tests ${SERVER_TEST_SOURCES} server_test_guard.cpp
               ${PROJECT_SOURCE_DIR}/tests/test_main.cpp)
target_link_libraries(tessera_server_tests PRIVATE tessera_server)
target_include_directories(tessera_server_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)

if(MSVC)
    target_compile_options(tessera_server_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_server_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME tessera_server_tests COMMAND tessera_server_tests)

# The example server over real sockets, and as a process that gets signals. Only with ENet.
if(TESSERA_BUILD_ENET)
    add_executable(tessera_server_enet_tests test_enet_server_example.cpp server_test_guard.cpp
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

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME tessera_example_server_process
                 COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/run_example_process.py
                         $<TARGET_FILE:tessera_example_server>)
        set_tests_properties(tessera_example_server_process PROPERTIES TIMEOUT 120)
    endif()
endif()
