project(
  'xsimd',
  'cpp',
  version: '14.3.0',
  license: 'BSD-3-Clause',
  default_options: ['cpp_std=c++14'],
  meson_version: '>=1.1.0',
)

xsimd_args = []
xsimd_deps = []

if get_option('enable_xtl_complex')
  xsimd_args += '-DXSIMD_ENABLE_XTL_COMPLEX=1'
  xsimd_deps += dependency('xtl', version: '>=0.8.0')
endif

if get_option('default_arch') != ''
  xsimd_args += '-DXSIMD_DEFAULT_ARCH=' + get_option('default_arch')
endif

xsimd_dep = declare_dependency(
  include_directories: include_directories('include'),
  compile_args: xsimd_args,
  dependencies: xsimd_deps,
  version: meson.project_version(),
)

meson.override_dependency('xsimd', xsimd_dep)

if not meson.is_subproject()
  install_subdir('include/xsimd', install_dir: get_option('includedir'))

  import('pkgconfig').generate(
    name: 'xsimd',
    description: 'xsimd provides a unified means for using SIMD features for library authors',
    extra_cflags: xsimd_args,
    install_dir: get_option('datadir') / 'pkgconfig',
  )
endif
