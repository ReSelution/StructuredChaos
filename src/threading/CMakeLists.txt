add_library(chaos_threading STATIC)

target_link_libraries(chaos_threading PUBLIC chaos_compiler_flags chaos_stats
                                             chaos_logger)
target_link_libraries(chaos_threading PRIVATE atomic_queue)

target_sources(
  chaos_threading
  PUBLIC FILE_SET
         CXX_MODULES
         FILES
         chaos_threading.cppm
         chaos_threading.impl.cppm
         chaos_spinlock.cppm
         chaos_atomic_queue.cppm)
