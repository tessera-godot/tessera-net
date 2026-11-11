# The head-to-head programs: a Tessera Net server and bot over ENet, run by run.py against the same scenario as
# bench/godot-replication. Built only with -DTESSERA_BUILD_ENET=ON.
foreach(program tessera_h2h_server tessera_h2h_bot)
    string(REPLACE "tessera_h2h_" "h2h_" source ${program})
    add_executable(${program} ${source}.cpp)
    target_link_libraries(${program} PRIVATE tessera_net_core tessera_net_enet)
    target_include_directories(${program} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(MSVC)
        target_compile_options(${program} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${program} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
