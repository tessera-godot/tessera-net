# Loopback tests of the ENet transport. Built only with -DTESSERA_BUILD_ENET=ON (see tests/CMakeLists.txt).
file(GLOB ENET_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_enet_*.cpp)
add_executable(tessera_net_enet_tests ${ENET_TEST_SOURCES} ${PROJECT_SOURCE_DIR}/tests/test_main.cpp)
target_link_libraries(tessera_net_enet_tests PRIVATE tessera_net_enet enet)
target_include_directories(tessera_net_enet_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_include_directories(tessera_net_enet_tests SYSTEM PRIVATE ${TESSERA_ENET_INCLUDE_DIR})
target_compile_definitions(tessera_net_enet_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

if(MSVC)
    target_compile_options(tessera_net_enet_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_net_enet_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME tessera_net_enet_tests COMMAND tessera_net_enet_tests)
