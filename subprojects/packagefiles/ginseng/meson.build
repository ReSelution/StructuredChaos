project(
  'ginseng',
  'cpp',
  version: '1.1',
  license: 'MIT',
  default_options: ['cpp_std=c++17'],
  meson_version: '>=1.1.0',
)

# Header-only.
ginseng_dep = declare_dependency(
  include_directories: include_directories('include', is_system: true),
  version: meson.project_version(),
)

meson.override_dependency('ginseng', ginseng_dep)
