# Compares the entity table with a general-purpose ECS (EnTT). Enabled with
# -DTESSERA_BUILD_ECS_COMPARISON=ON; the first configure downloads EnTT.
include(FetchContent)
FetchContent_Declare(entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG v3.16.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(entt)

add_executable(tessera_net_ecs_comparison main.cpp)
target_link_libraries(tessera_net_ecs_comparison PRIVATE tessera_net_core EnTT::EnTT)
if(MSVC)
    target_compile_options(tessera_net_ecs_comparison PRIVATE /W4 /permissive-)
else()
    target_compile_options(tessera_net_ecs_comparison PRIVATE -Wall -Wextra -Wpedantic)
endif()
