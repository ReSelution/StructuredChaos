add_library(chaos_stats STATIC)

target_sources(
  chaos_stats
  PUBLIC FILE_SET
         CXX_MODULES
         FILES
         timer.cppm
         stats.cppm
         units.cppm
         stats_core.cppm
         stats.impl.cppm
         throughput.cppm
         counter.cppm

)

target_link_libraries(chaos_stats PUBLIC chaos_compiler_flags chaos_util)


