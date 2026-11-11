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
