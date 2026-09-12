cmake_minimum_required(VERSION 4.3)

set(CMAKE_EXPERIMENTAL_EXPORT_BUILD_DATABASE
    "73194a1d-c0b5-41b9-9190-a4512925e192")
project(StructuredChaos LANGUAGES CXX)

include(CheckCXXCompilerFlag)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

target_link_libraries(
  StructuredChaos
  PUBLIC chaos_compiler_flags
         chaos_logger
         chaos_stats
         chaos_hash
         chaos_memory
         chaos_threading
         mimalloc-static)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(SAA_TARGETING_WINDOWS TRUE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(StructuredChaos PUBLIC "-static" "-static-libgcc"
                        "-static-libstdc++")
  endif()

endif()


if(PROJECT_IS_TOP_LEVEL)
  message(
    STATUS "[SC] Building as main project - enabling test suite & sandbox")
  enable_testing()
  add_subdirectory(tests)
else()
  message(STATUS "[SC] Building as dependency - skipping tests and sandbox")
endif()
