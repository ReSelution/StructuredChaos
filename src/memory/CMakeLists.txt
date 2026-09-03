add_library(chaos_memory STATIC)

target_link_libraries(chaos_memory PUBLIC chaos_compiler_flags chaos_stats chaos_threading)

target_sources(
  chaos_memory
  PUBLIC FILE_SET
         CXX_MODULES
         FILES
         memory.cppm
         monotonic_res.cppm
         arena.cppm
         arena_pool.cppm)
