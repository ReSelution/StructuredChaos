add_library(chaos_logger STATIC)

target_sources(chaos_logger PUBLIC FILE_SET CXX_MODULES FILES chaos_logger.cppm
                                   chaos_logger.impl.cppm)
target_link_libraries(chaos_logger PUBLIC chaos_compiler_flags chaos_util)
target_link_libraries(chaos_logger PRIVATE spdlog)
