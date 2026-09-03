if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(OLD_OUTPUT_DIR "${CMAKE_BINARY_DIR}")
else()
  set(OLD_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()

macro(sc_add_test_executable TARGET_NAME SOURCE_FILE)
  add_executable(${TARGET_NAME} ${SOURCE_FILE})
  target_link_libraries(${TARGET_NAME} PRIVATE StructuredChaos)

  # Wir setzen das Verzeichnis und nutzen die konfigurationsspezifische
  # Eigenschaft für den Ausgabenamen (z.B. QueueTest-Debug)
  set_target_properties(
    ${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OLD_OUTPUT_DIR}/tests"
                              OUTPUT_NAME "${TARGET_NAME}-$<CONFIG>")

  add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
endmacro()

# --- Registrierung deiner Tests ---
sc_add_test_executable(LoggerTest logger/logger_test.cpp)
sc_add_test_executable(StatsTest stats/stats_test.cpp)
sc_add_test_executable(HashTest hash/hash_test.cpp)
sc_add_test_executable(ThreadingTest threading/threading_test.cpp)
sc_add_test_executable(QueueTest threading/queue_test.cpp)

target_link_libraries(QueueTest PRIVATE atomic_queue)
