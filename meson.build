project(
  'StructuredChaos',
  'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++23'],
  meson_version: '>= 1.1.0',
)

cmake = import('cmake')

# Dependency
entt_dep = dependency('entt', fallback: ['entt', 'entt_dep'])
spdlog_dep = dependency('spdlog', fallback: ['spdlog', 'spdlog_dep'])
unordered_dense_dep = dependency(
  'unordered_dense',
  fallback: ['unordered_dense', 'unordered_dense_dep'],
)

mi_opts = cmake.subproject_options()
mi_opts.add_cmake_defines(
  {
    'MI_BUILD_TESTS': false,
    'MI_BUILD_SHARED': false,
    'MI_BUILD_OBJECT': false,
  },
)

mimalloc_proj = cmake.subproject('mimalloc', options: mi_opts)
mimalloc_dep = mimalloc_proj.dependency('mimalloc-static')

glm_proj = subproject('glm')
glm_dep = glm_proj.get_variable('glm_dep')

xsimd_dep = dependency('xsimd', fallback: ['xsimd', 'xsimd_dep'])

simdutf_proj = subproject(
  'simdutf',
)
simdutf_dep = simdutf_proj.get_variable('simdutf_dep')
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

#Config
conf_data = configuration_data()

if get_option('enable_chaos_stats')
  conf_data.set('CHAOS_STATS_ENABLED', 1)
endif

if get_option('dump_magpie')
  conf_data.set('DUMP_MAGPIE', 1)
endif

config_h = configure_file(
  input: 'src/sc/config/config.h.in',
  output: 'config.h',
  configuration: conf_data,
  install: false,
)

inc_src = include_directories('src')

chaos_config_dep = declare_dependency(sources: config_h, include_directories: inc_src)

subdir('src')

run_command(
  'ln',
  '-sf', meson.project_build_root() / 'compile_commands.json',
  meson.project_source_root() / 'compile_commands.json',
  check: false,
)

subdir('tests')
