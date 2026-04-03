cmake_minimum_required(VERSION 3.24)

if(TARGET StructuredChaos)
    return()
endif()

project(StructuredChaos)

set(CMAKE_CXX_STANDARD 20)

include(cmake/dependencies.cmake)

option(ENABLE_CHAOS_STATS "Enable Chaos Stats" ON)

add_library(StructuredChaos STATIC
        src/memory/chaos_memory_resource.cpp
        src/memory/chaos_memory_resource.hpp
)

target_sources(StructuredChaos PRIVATE
        src/memory/chaos_bump_arena.cpp
        src/memory/chaos_arena_pool.cpp

        src/threading/chaos_threading.cpp

        src/ecs/chaos_registry.cpp
        src/ecs/chaos_entity.cpp

        src/memory/chaos_monotonic_resource.cpp
        src/threading/chaos_spin_lock.cpp
)


target_sources(StructuredChaos PUBLIC
        src/logger/chaos_logger.hpp

        src/memory/chaos_monotonic_resource.hpp
        src/memory/chaos_tls_arena.hpp
        src/memory/chaos_sharded_cache.hpp
        src/memory/chaos_bump_arena.hpp
        src/memory/chaos_arena_pool.hpp

        src/ecs/chaos_registry.hpp
        src/ecs/chaos_entity.hpp

        src/stats/chaos_timer.hpp
        src/stats/chaos_stats.hpp
        src/stats/chaos_throughput.hpp
        src/stats/chaos_units.hpp
        src/stats/chaos_counter.hpp
        src/stats/chaos_global.hpp
        src/stats/chaos_map_counter.hpp

        src/threading/chaos_threading.hpp
        src/threading/chaos_spin_lock.hpp
)

if(NOT MSVC)
    target_compile_options(StructuredChaos PUBLIC -mcx16 -march=native)
else()
    target_compile_options(StructuredChaos PUBLIC /arch:AVX2)
endif()

add_subdirectory(external/tracy)
target_link_libraries(StructuredChaos
        PUBLIC
        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
        TracyClient
        atomic
        EnTT
)

target_include_directories(StructuredChaos
        PUBLIC
        src
)

if(ENABLE_CHAOS_STATS)
    target_compile_definitions(StructuredChaos PUBLIC CHAOS_STATS_ENABLED)
    message(STATUS "[SC] STATS Enabled")
endif ()



if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(SC_IS_MAIN_PROJECT ON)
else()
    set(SC_IS_MAIN_PROJECT OFF)
endif()

if(SC_IS_MAIN_PROJECT)
    message(STATUS "[SC] Building as main project - enabling tests")
    add_executable(StructuredChaosTest src/main.cpp)
    target_link_libraries(StructuredChaosTest PRIVATE StructuredChaos)
else()
    message(STATUS "[SC] Building as subdirectory - skipping tests")
endif()