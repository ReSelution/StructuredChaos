add_library(chaos_memory STATIC)

target_link_libraries(chaos_memory PUBLIC chaos_compiler_flags chaos_stats chaos_threading)

target_sources(
  chaos_memory
  PUBLIC FILE_SET
         CXX_MODULES
         FILES
         chaos_memory.cppm
         chaos_monotonic_res.cppm
         chaos_arena.cppm
         chaos_arena_pool.cppm)
