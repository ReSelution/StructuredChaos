cmake_minimum_required(VERSION 3.24)

project(StructuredChaos LANGUAGES CXX)

find_program(CCACHE_PROGRAM ccache)
if (CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "ccache gefunden und aktiviert.")
endif ()
include(CheckCXXCompilerFlag)


set(CMAKE_CXX_STANDARD 23)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


add_library(StructuredChaos STATIC
        src/hash/hash.cpp
        src/hash/hash.hpp
        src/localizations/magpie_translator.cpp
        src/localizations/magpie_string.cpp
        src/localizations/magpie_string.hpp
)

if (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(SAA_TARGETING_WINDOWS TRUE)
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_link_options("-static")
        add_link_options("-static-libgcc")
        add_link_options("-static-libstdc++")
    endif ()
else ()
    set(SAA_TARGETING_WINDOWS FALSE)
endif ()

if (NOT MSVC)
    include(CheckCXXCompilerFlag)

    set(_SAA_LINKER "Default (Slow)")

    if (NOT SAA_TARGETING_WINDOWS)
        check_cxx_compiler_flag("-fuse-ld=mold" SAA_HAS_MOLD)
        check_cxx_compiler_flag("-fuse-ld=lld" SAA_HAS_LLD)
    endif ()

    if (SAA_HAS_MOLD AND NOT SAA_TARGETING_WINDOWS)
        set(_SAA_LINKER "mold")
        add_link_options("-fuse-ld=mold")
    elseif (SAA_HAS_LLD AND NOT SAA_TARGETING_WINDOWS)
        set(_SAA_LINKER "lld")
        add_link_options("-fuse-ld=lld")
    endif ()

    message(STATUS "[CHAOS] Using Linker: ${_SAA_LINKER}")
endif ()

if (NOT MSVC)
    if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        if (MINGW)
            target_compile_options(StructuredChaos PUBLIC -O3 -fno-rtti -march=x86-64-v2)
            message(STATUS "[SAA] MinGW: Using x86-64-v2 instead of native")
        else()
            target_compile_options(StructuredChaos PUBLIC -O3 -march=native -fno-rtti)
        endif()
    else ()
        target_compile_options(StructuredChaos PUBLIC -g -O0)
        message(STATUS "[SAA] Debug mode: Optimizations disabled")
    endif ()
endif ()


include(cmake/dependencies.cmake)
add_subdirectory(external/rapidhash)
option(ENABLE_CHAOS_STATS "Enable Chaos Stats" ON)
option(DUMP_MAGPIE "DUMP_MAGPIE" ON)


include(CheckIPOSupported)
check_ipo_supported(RESULT SAA_LTO_SUPPORTED OUTPUT error)
if (SAA_LTO_SUPPORTED AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT MINGW)
    set_target_properties(StructuredChaos PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "[Chaos] LTO enabled")
else ()
    message(STATUS "[Chaos] LTO skipped: ${error}")
endif ()

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
#set_source_files_properties(src/hash/hash.cpp PROPERTIES COMPILE_FLAGS "-O3 -march=native")
if (NOT MSVC)
    target_compile_options(StructuredChaos PUBLIC -mcx16 -march=native)
else ()
    target_compile_options(StructuredChaos PUBLIC /arch:AVX2)
endif ()

target_link_libraries(StructuredChaos
        PUBLIC
        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
        TracyClient
        mimalloc-static
        atomic
        EnTT
        pfr
        hwy
        rapidHash
        unordered_dense::unordered_dense
        glm::glm
        PRIVATE
        simdutf
        atomic_queue::atomic_queue
)

target_include_directories(StructuredChaos
        PUBLIC
        src
)

if (ENABLE_CHAOS_STATS)
    target_compile_definitions(StructuredChaos PUBLIC CHAOS_STATS_ENABLED)
endif ()

if (DUMP_MAGPIE)
    target_compile_definitions(StructuredChaos PUBLIC DUMP_MAGPIE)
endif ()


if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(SC_IS_MAIN_PROJECT ON)
else ()
    set(SC_IS_MAIN_PROJECT OFF)
endif ()

if (SC_IS_MAIN_PROJECT)
    message(STATUS "[SC] Building as main project - enabling test suite")
    enable_testing()
    add_subdirectory(tests)

    add_executable(StructuredChaosSandbox src/main.cpp)
    target_link_libraries(StructuredChaosSandbox PRIVATE StructuredChaos)
else ()
    message(STATUS "[SC] Building as subdirectory - skipping tests")
endif ()