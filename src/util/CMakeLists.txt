add_library(chaos_util)

target_sources(chaos_util PUBLIC FILE_SET CXX_MODULES FILES chaos_util.cppm
                                 fixed_string.cppm utf.cppm)

target_link_libraries(chaos_util PUBLIC chaos_compiler_flags simdutf)
