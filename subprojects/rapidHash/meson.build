project('rapidHash', 'cpp', version: '0.1', meson_version: '>=0.50.0')

rapidHash_inc = include_directories('.')

rapidHash_dep = declare_dependency(
  include_directories: rapidHash_inc,
  sources: files('rapidhash-constexpr.h', 'rapidhash.h'),
)
