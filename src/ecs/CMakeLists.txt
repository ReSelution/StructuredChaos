add_library(chaos_ecs STATIC)

target_sources(
  chaos_ecs
  PUBLIC FILE_SET
         CXX_MODULES
         FILES
         ecs.cppm
         registery.cppm
         anchor.cppm
         entity.cppm)

target_link_libraries(chaos_ecs PUBLIC chaps_compiler_flags chaos_stats EnTT
                                       pfr)

target_sources(chaos_ecs PUBLIC FILE_SET CXX_MODULES FILES resource.cppm
                                view.cppm)
