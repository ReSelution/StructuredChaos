cmake_minimum_required(VERSION 3.16)
project(xxHASH LANGUAGES C)
set(CMAKE_CXX_STANDARD 20)


add_library(xxHASH STATIC xxh3.h xxhash.h xxhash.c constexpr-xxh3.h)

target_include_directories(xxHASH
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
