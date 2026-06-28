add_library(chaos_hash STATIC)

target_link_libraries(chaos_hash PUBLIC chaos_compiler_flags rapidHash)

target_sources(chaos_hash PUBLIC FILE_SET CXX_MODULES FILES chaos_hash.cppm
                                 chaos_hash.impl.cppm)
