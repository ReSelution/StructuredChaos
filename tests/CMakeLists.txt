# tests/CMakeLists.txt

add_executable(RegistryStressTest registry_stress_test.cpp)
target_link_libraries(RegistryStressTest PRIVATE StructuredChaos)

add_executable(HashTest hash/hash_test.cpp)
target_link_libraries(HashTest PRIVATE StructuredChaos)

add_test(NAME RegistryStress COMMAND RegistryStressTest)
add_test(NAME HashTest COMMAND RegistryStressTest)