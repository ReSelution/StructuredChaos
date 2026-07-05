add_library(chaos_stats STATIC)

target_sources(
  chaos_stats
  PUBLIC FILE_SET
         CXX_MODULES
         FILES
         chaos_timer.cppm
         chaos_stats.cppm
         chaos_units.cppm
         chaos_stats_core.cppm)

target_link_libraries(chaos_stats PUBLIC chaos_compiler_flags chaos_util)

target_sources(
  chaos_stats PUBLIC FILE_SET CXX_MODULES FILES chaos_stats.impl.cppm
                     chaos_throughput.cppm chaos_counter.cppm)

