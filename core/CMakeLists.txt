find_package(Threads REQUIRED)

add_library(tessera_net_core STATIC
    src/version.cpp
    src/job_system.cpp
    src/spatial_grid.cpp
    src/tick_loop.cpp
    src/relevancy/relevancy_config.cpp
    src/relevancy/compact_ids.cpp
    src/relevancy/relevancy_groups.cpp
    src/relevancy/visible_set.cpp
    src/relevancy/relevancy_engine.cpp
    src/replication/bit_stream.cpp
    src/replication/property_schema.cpp
    src/replication/property_codecs.cpp
    src/replication/state_store.cpp
    src/replication/connection_state.cpp
    src/replication/record_codec.cpp
    src/replication/receiver_state.cpp
    src/replication/replication_engine.cpp
    src/replication/subscription_index.cpp
    src/transport/simulated_network.cpp
    src/protocol.cpp
    src/client_replica.cpp
    src/predictor.cpp
    src/replication_server.cpp)

target_include_directories(tessera_net_core PUBLIC include)
target_compile_features(tessera_net_core PUBLIC cxx_std_20)
target_link_libraries(tessera_net_core PUBLIC Threads::Threads)

# Measurement builds only: the stage timers of tessera/net/profile.hpp. PUBLIC so that the tests and the benchmark
# see the same setting as the library.
if(TESSERA_PROFILE)
    target_compile_definitions(tessera_net_core PUBLIC TESSERA_PROFILE=1)
endif()

if(MSVC)
    target_compile_options(tessera_net_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_net_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The ENet transport (docs/adr/0003-library-policy.md: an optional library, behind our own interface). It exists
# only with -DTESSERA_BUILD_ENET=ON; the first configure then downloads ENet.
if(TESSERA_BUILD_ENET)
    include(FetchContent)
    FetchContent_Declare(enet
        GIT_REPOSITORY https://github.com/lsalzman/enet.git
        # The commit of the tag v1.3.18, so that what is fetched does not depend on the tag staying where it is.
        GIT_TAG 2662c0de09e36f2a2030ccc2c528a3e4c9e8138a)
    # ENet's own code is built without warnings (they are not ours to fix). Its default /W3 is dropped first, so
    # that /w does not make the compiler warn about replacing it; the function keeps that change local.
    function(tessera_fetch_enet)
        if(MSVC)
            string(REPLACE "/W3" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
            string(REPLACE "/W3" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
            string(REPLACE "/W3" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
        endif()
        FetchContent_MakeAvailable(enet)
        set(TESSERA_ENET_INCLUDE_DIR "${enet_SOURCE_DIR}/include" CACHE INTERNAL "ENet headers, for the tests and the benchmarks")
        if(MSVC)
            target_compile_options(enet PRIVATE /w)
        endif()
    endfunction()
    tessera_fetch_enet()

    add_library(tessera_net_enet STATIC src/transport/enet_transport.cpp)
    target_link_libraries(tessera_net_enet PUBLIC tessera_net_core PRIVATE enet)
    # SYSTEM: ENet's own headers are not ours to fix warnings in.
    target_include_directories(tessera_net_enet SYSTEM PRIVATE ${TESSERA_ENET_INCLUDE_DIR})
    target_compile_definitions(tessera_net_enet PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(WIN32)
        target_link_libraries(tessera_net_enet PUBLIC ws2_32 winmm) # ENet's own target does not carry them
    endif()
    if(MSVC)
        target_compile_options(tessera_net_enet PRIVATE /W4 /permissive-)
    else()
        target_compile_options(tessera_net_enet PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
