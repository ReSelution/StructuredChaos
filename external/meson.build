# Dependency
# 64-bit ids give 2^32 entities per registry instead of 2^20.
entt_dep = dependency(
  'entt',
  fallback: ['entt', 'entt_dep'],
  default_options: ['use_atomic=true', 'id_type=std::uint64_t'],
)
spdlog_dep = dependency(
  'spdlog',
  fallback: ['spdlog', 'spdlog_dep'],
  default_options: ['default_library=static'],
)
unordered_dense_dep = dependency(
  'unordered_dense',
  fallback: ['unordered_dense', 'unordered_dense_dep'],
)

# The ECS uses first-class mimalloc heaps, which are not reliable before 3.5.3.
mimalloc_dep = dependency(
  'mimalloc',
  version: '>=3.5.3',
  fallback: ['mimalloc', 'mimalloc_static_dep'],
  default_options: [
    'mi_build_shared=false',
    'mi_build_object=false',
    'mi_build_tests=false',
  ],
)

glm_dep = dependency('glm', fallback: ['glm', 'glm_dep'])

xsimd_dep = dependency('xsimd', fallback: ['xsimd', 'xsimd_dep'])

simdutf_dep = dependency(
  'simdutf',
  fallback: ['simdutf', 'simdutf_dep'],
  default_options: ['default_library=static'],
)
rapidHash_dep = dependency('rapidHash', fallback: ['rapidHash', 'rapidHash_dep'])
pfr_dep = dependency('pfr', fallback: ['pfr', 'pfr_dep'])

all_deps = [
  entt_dep,
  spdlog_dep,
  unordered_dense_dep,
  mimalloc_dep,
  glm_dep,
  xsimd_dep,
  simdutf_dep,
  pfr_dep,
  rapidHash_dep,
]
