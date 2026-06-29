add_executable(LoggerTest logger/logger_test.cpp)

get_target_property(OLD_OUTPUT_DIR LoggerTest RUNTIME_OUTPUT_DIRECTORY)
if(NOT OLD_OUTPUT_DIR)
  set(OLD_OUTPUT_DIR "${CMAKE_BINARY_DIR}")
endif()
target_link_libraries(LoggerTest PRIVATE StructuredChaos)
set_target_properties(LoggerTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                            "${OLD_OUTPUT_DIR}/tests")
add_test(NAME LoggerTest COMMAND LoggerTest)

add_executable(StatsTest stats/stats_test.cpp)
target_link_libraries(StatsTest PRIVATE StructuredChaos)
set_target_properties(StatsTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                           "${OLD_OUTPUT_DIR}/tests")
add_test(NAME StatsTest COMMAND StatsTest)

add_executable(HashTest hash/hash_test.cpp)
target_link_libraries(HashTest PRIVATE StructuredChaos)
set_target_properties(HashTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                          "${OLD_OUTPUT_DIR}/tests")
add_test(NAME HashTest COMMAND HashTest)


add_executable(ThreadingTest threading/threading_test.cpp)
target_link_libraries(ThreadingTest PRIVATE StructuredChaos)
set_target_properties(ThreadingTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                          "${OLD_OUTPUT_DIR}/tests")
add_test(NAME ThreadingTest COMMAND ThreadingTest)
