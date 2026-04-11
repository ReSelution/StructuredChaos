cmake_minimum_required(VERSION 3.16)
project(CITY_HASH LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

set(CITY_HASH_SOURCE
        city.cc
        city.h
        citycrc.h
        config.h
)

add_library(CITY_HASH STATIC ${CITY_HASH_SOURCE})

target_include_directories(CITY_HASH
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
