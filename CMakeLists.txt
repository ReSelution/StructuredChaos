cmake_minimum_required(VERSION 4.1)

if(TARGET StructuredChaos)
    return()
endif()

project(StructuredChaos)

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(external/spdlog)

add_library(StructuredChaos STATIC)

target_sources(StructuredChaos PRIVATE
        src/stats/chaos_global.cpp
        src/cache/chaos_slab.cpp
        src/cache/chaos_slab_pool.cpp
        src/threading/chaos_threading.cpp
)

target_sources(StructuredChaos PUBLIC
        src/logger/chaos_logger.hpp
        src/stats/chaos_timer.hpp
        src/cache/chaos_tls_slab.hpp
        src/cache/chaos_sharded_cache.hpp
        src/stats/chaos_stats.hpp
        src/stats/chaos_throughput.hpp
        src/stats/chaos_units.hpp
        src/stats/chaos_counter.hpp
        src/stats/chaos_global.hpp
        src/cache/chaos_slab.hpp
        src/stats/chaos_map_counter.hpp
        src/cache/chaos_slab_pool.hpp
        src/threading/chaos_threading.hpp

)

target_link_libraries(StructuredChaos PUBLIC spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>)
target_include_directories(StructuredChaos
        PUBLIC
        src
)

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