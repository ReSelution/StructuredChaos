add_library(rapidHash INTERFACE rapidhash.h rapidhash-constexpr.h)

target_include_directories(rapidHash INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

