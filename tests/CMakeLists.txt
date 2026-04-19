# tests/CMakeLists.txt

add_executable(RegistryStressTest ecs/registry_stress_test.cpp)
target_link_libraries(RegistryStressTest PRIVATE StructuredChaos)
add_test(NAME RegistryStress COMMAND RegistryStressTest)

add_executable(HashTest hash/hash_test.cpp)
target_link_libraries(HashTest PRIVATE StructuredChaos simdutf)
add_test(NAME HashTest COMMAND HashTest)

add_executable(ThreadingTest threading/threading_test.cpp)
target_link_libraries(ThreadingTest PRIVATE StructuredChaos)
add_test(NAME ThreadingTest COMMAND ThreadingTest)

