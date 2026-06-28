set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.fetch_cache")
include(FetchContent)

# ==========================================
# 1. EnTT & spdlog & pfr & unordered_dense
# ==========================================
FetchContent_Declare(
  EnTT
  GIT_REPOSITORY https://github.com/skypjack/entt.git
  GIT_TAG v3.16.0)
FetchContent_MakeAvailable(EnTT)

FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.17.0)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
  pfr
  GIT_REPOSITORY https://github.com/apolukhin/pfr_non_boost.git
  GIT_TAG 2.3.2)
FetchContent_MakeAvailable(pfr)

FetchContent_Declare(
  unordered_dense
  GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
  GIT_TAG v4.8.1)
FetchContent_MakeAvailable(unordered_dense)

FetchContent_Declare(
  atomic_queue
  GIT_REPOSITORY https://github.com/max0x7ba/atomic_queue.git
  GIT_TAG v1.7.3)
FetchContent_MakeAvailable(atomic_queue)

# ==========================================
# 1. mimalloc
# ==========================================
set(MI_BUILD_TESTS
    OFF
    CACHE BOOL "Disable mimalloc tests" FORCE)
set(MI_BUILD_SHARED
    OFF
    CACHE BOOL "Build mimalloc as static library" FORCE)
set(MI_BUILD_OBJECT
    OFF
    CACHE BOOL "Disable mimalloc object build" FORCE)

FetchContent_Declare(
  mimalloc
  GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
  GIT_TAG v3.2.8)
FetchContent_MakeAvailable(mimalloc)

# ==========================================
# 1. GLM
# ==========================================
set(GLM_ENABLE_CXX_20
    ON
    CACHE BOOL "Enable C++20 for GLM" FORCE)
set(GLM_BUILD_TESTS
    OFF
    CACHE BOOL "Disable GLM tests" FORCE)
set(GLM_BUILD_INSTALL
    OFF
    CACHE BOOL "Disable GLM installation" FORCE)

FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.3)
FetchContent_MakeAvailable(glm)

FetchContent_Declare(
  xsimd
  GIT_REPOSITORY https://github.com/xtensor-stack/xsimd.git
  GIT_TAG 14.2.0)
FetchContent_MakeAvailable(xsimd)

# ==========================================
# 1. simdutf
# ==========================================
set(SIMDUTF_TOOLS
    OFF
    CACHE BOOL "Disable simdutf tools" FORCE)
set(SIMDUTF_TESTS
    OFF
    CACHE BOOL "Disable simdutf tests" FORCE)
set(SIMDUTF_BENCHMARKS
    OFF
    CACHE BOOL "Disable simdutf benchmarks" FORCE)

FetchContent_Declare(
  simdutf
  GIT_REPOSITORY https://github.com/simdutf/simdutf.git
  GIT_TAG v8.2.0)
FetchContent_MakeAvailable(simdutf)
