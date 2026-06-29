cmake_minimum_required(VERSION 4.3)
project(StructuredChaos LANGUAGES CXX)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()
include(CheckCXXCompilerFlag)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPERIMENTAL_EXPORT_BUILD_DATABASE
    "73194a1d-c0b5-41b9-9190-a4512925e192")
set(CMAKE_EXPORT_BUILD_DATABASE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Qunused-arguments")
endif()
add_library(chaos_compiler_flags INTERFACE)

if(NOT MSVC)
  target_compile_options(
    chaos_compiler_flags INTERFACE -fno-rtti $<$<CONFIG:Debug>:-g -O0>
                                   $<$<NOT:$<CONFIG:Debug>>:-O3>)
  if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(MINGW)
      target_compile_options(chaos_compiler_flags INTERFACE -march=x86-64-v2)
    else()
      target_compile_options(chaos_compiler_flags INTERFACE -march=native)
    endif()
  endif()
endif()

option(ENABLE_CHAOS_STATS "Enable Chaos Stats" ON)
option(DUMP_MAGPIE "DUMP_MAGPIE" OFF)

target_compile_definitions(
  chaos_compiler_flags
  INTERFACE $<$<BOOL:${ENABLE_CHAOS_STATS}>:CHAOS_STATS_ENABLED>
            $<$<BOOL:${DUMP_MAGPIE}>:DUMP_MAGPIE>)

include(cmake/dependencies.cmake)
add_subdirectory(external/rapidhash)
add_subdirectory(src)

add_library(StructuredChaos STATIC)

target_sources(
  StructuredChaos PUBLIC FILE_SET CXX_MODULES BASE_DIRS
                         ${CMAKE_CURRENT_SOURCE_DIR}/src FILES src/sc.cppm)

target_link_libraries(StructuredChaos PUBLIC chaos_compiler_flags chaos_logger
                                             chaos_stats chaos_hash chaos_memory chaos_threading mimalloc-static)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(SAA_TARGETING_WINDOWS TRUE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(StructuredChaos PUBLIC "-static" "-static-libgcc"
                        "-static-libstdc++")
  endif()
else()
  set(SAA_TARGETING_WINDOWS FALSE)
endif()

if(NOT MSVC AND NOT SAA_TARGETING_WINDOWS)
  set(SAA_LINKER "Default")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-fuse-ld=mold" SAA_HAS_MOLD)
    check_cxx_compiler_flag("-fuse-ld=lld" SAA_HAS_LLD)

    if(SAA_HAS_MOLD)
      set(SAA_LINKER "mold")
    elseif(SAA_HAS_LLD)
      set(SAA_LINKER "lld")
    endif()

    if(NOT SAA_LINKER STREQUAL "Default")
      target_link_options(StructuredChaos PUBLIC "-fuse-ld=${SAA_LINKER}")
    endif()
  endif()
  message(STATUS "[CHAOS] Using Linker: ${SAA_LINKER}")
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT SAA_LTO_SUPPORTED OUTPUT SAA_LTO_ERROR)

if(SAA_LTO_SUPPORTED AND NOT MINGW)
  set_target_properties(
    StructuredChaos PROPERTIES INTERPROCEDURAL_OPTIMIZATION
                               $<$<NOT:$<CONFIG:Debug>>:TRUE>)
  message(
    STATUS "[Chaos] LTO supported (Active in Release/MinSizeRel/RelWithDebInfo)"
  )
else()
  message(STATUS "[Chaos] LTO skipped: ${SAA_LTO_ERROR}")
endif()

if(PROJECT_IS_TOP_LEVEL)
  message(
    STATUS "[SC] Building as main project - enabling test suite & sandbox")
  enable_testing()
  add_subdirectory(tests)
else()
  message(STATUS "[SC] Building as dependency - skipping tests and sandbox")
endif()
