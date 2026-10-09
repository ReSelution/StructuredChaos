project(
  'gaia-ecs',
  'cpp',
  version: '1.0.0',
  license: 'MIT',
  default_options: ['cpp_std=c++17'],
  meson_version: '>=1.1.0',
)

# Header-only. The thread pool behind the parallel queries needs threads.
gaia_ecs_dep = declare_dependency(
  include_directories: include_directories('include', is_system: true),
  dependencies: [dependency('threads')],
  version: meson.project_version(),
)

meson.override_dependency('gaia-ecs', gaia_ecs_dep)
