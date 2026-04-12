cmake_minimum_required(VERSION 3.16)
project(rapidHash LANGUAGES CXX)

add_library(rapidHash INTERFACE)

target_include_directories(rapidHash
        INTERFACE "${CMAKE_SOURCE_DIR}/external"
)