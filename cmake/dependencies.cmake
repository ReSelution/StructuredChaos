# ==========================================
# 1. CPM-Zentraler Cache Setup
# ==========================================
set(MY_FETCH_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/.fetch_cache")

set(CPM_SOURCE_CACHE "${MY_FETCH_CACHE}" CACHE PATH "CPM Cache" FORCE)
set(CPM_DOWNLOAD_ALL SYSTEM CACHE BOOL "" FORCE)
include("${CMAKE_CURRENT_LIST_DIR}/CPM.cmake")

# ==========================================
# 2. EnTT & spdlog & unordered_dense & atomic_queue
# ==========================================
CPMAddPackage("gh:skypjack/entt@3.16.0")
CPMAddPackage("gh:gabime/spdlog@1.17.0")
CPMAddPackage("gh:martinus/unordered_dense@4.8.1")
CPMAddPackage("gh:max0x7ba/atomic_queue@1.7.3")

# ==========================================
# 3. mimalloc
# ==========================================
set(MI_BUILD_TESTS OFF CACHE BOOL "Disable mimalloc tests" FORCE)
set(MI_BUILD_SHARED OFF CACHE BOOL "Build mimalloc as static library" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "Disable mimalloc object build" FORCE)

CPMAddPackage("gh:microsoft/mimalloc@3.2.8")

# ==========================================
# 4. GLM & xsimd
# ==========================================
set(GLM_ENABLE_CXX_20 ON CACHE BOOL "Enable C++20 for GLM" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "Disable GLM tests" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "Disable GLM installation" FORCE)

CPMAddPackage(
    NAME glm
    GIT_REPOSITORY "https://github.com/g-truc/glm.git"
    GIT_TAG "1.0.3"
)

CPMAddPackage(
    NAME xsimd
    GIT_REPOSITORY "https://github.com/xtensor-stack/xsimd.git"
    GIT_TAG "14.2.0"
)
# ==========================================
# 5. simdutf
# ==========================================
set(SIMDUTF_TOOLS OFF CACHE BOOL "Disable simdutf tools" FORCE)
set(SIMDUTF_TESTS OFF CACHE BOOL "Disable simdutf tests" FORCE)
set(SIMDUTF_BENCHMARKS OFF CACHE BOOL "Disable simdutf benchmarks" FORCE)

CPMAddPackage("gh:simdutf/simdutf@9.0.0")
