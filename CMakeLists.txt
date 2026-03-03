cmake_minimum_required(VERSION 4.1)
project(StructuredChaos)

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(external/spdlog)

add_library(StructuredChaos STATIC
        src/logger/chaos_logger.cpp
        src/logger/chaos_logger.hpp
        src/stats/chaos_timer.cpp
        src/stats/chaos_timer.hpp
)

target_link_libraries(StructuredChaos PUBLIC spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>)
target_include_directories( StructuredChaos
        PUBLIC
        src
)

add_executable(StructuredChaosTest src/main.cpp )
target_link_libraries(StructuredChaosTest PRIVATE StructuredChaos)
