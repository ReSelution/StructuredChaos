add_library(chaos_threading STATIC)

target_link_libraries(chaos_threading PUBLIC chaos_compiler_flags chaos_stats
                                             chaos_logger)
target_link_libraries(chaos_threading PRIVATE atomic_queue)

target_sources(
  chaos_threading
  PUBLIC FILE_SET
         CXX_MODULES
         FILES
         threading.cppm
         spinlock.cppm
         atomic_queue.cppm
         thrd_types.cppm
         thrd_backend.cppm
         thrd_dispatcher.cppm
         thrd_internal.cppm
         thrd_enqueue.cppm
         thrd_detach.cppm)

target_sources(chaos_threading PUBLIC FILE_SET CXX_MODULES FILES
                                      thrd_interface.cppm future_group.cppm)
