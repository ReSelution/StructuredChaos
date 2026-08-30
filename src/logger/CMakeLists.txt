add_library(chaos_logger STATIC)

target_sources(chaos_logger PUBLIC FILE_SET CXX_MODULES FILES logger.cppm
                                   logger.impl.cppm)
target_link_libraries(chaos_logger PUBLIC chaos_compiler_flags chaos_util chaos_stats)
target_link_libraries(chaos_logger PRIVATE spdlog)
