# cmake/linux-toolchain.cmake

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Compilers
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

# Detect preferred fast linker (mold -> lld -> default)
execute_process(
  COMMAND mold --version
  RESULT_VARIABLE SAA_MOLD_CHECK
  OUTPUT_QUIET ERROR_QUIET
)

execute_process(
  COMMAND lld --version
  RESULT_VARIABLE SAA_LLD_CHECK
  OUTPUT_QUIET ERROR_QUIET
)

if(SAA_MOLD_CHECK EQUAL 0)
  set(SAA_LINKER "mold")
elseif(SAA_LLD_CHECK EQUAL 0)
  set(SAA_LINKER "lld")
else()
  set(SAA_LINKER "")
endif()

# Apply linker and LTO flags
if(SAA_LINKER)
  set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=${SAA_LINKER}")
  set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=${SAA_LINKER}")
  set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=${SAA_LINKER}")
  message(STATUS "[CHAOS] Toolchain Linker: ${SAA_LINKER}")
endif()

add_compile_options(-flto=thin)
add_link_options(-flto=thin)
