project(
  'pfr',
  'cpp',
  version: '2.2.0',
  license: 'BSL-1.0',
  meson_version: '>=1.3.0',
)

pfr_use_modules = get_option('use_modules')

pfr_inc = include_directories('include')

if pfr_use_modules
  pfr_lib = library(
    'pfr',
    sources: files('modules/boost_pfr.cppm'),
    include_directories: pfr_inc,
    cpp_args: ['-DPFR_USE_MODULES'],
  )

  pfr_dep = declare_dependency(
    link_with: pfr_lib,
    include_directories: pfr_inc,
    compile_args: ['-DPFR_USE_MODULES'],
  )
else
  pfr_dep = declare_dependency(
    include_directories: pfr_inc,
  )
endif

meson.override_dependency('pfr', pfr_dep)

# Testing (only built when executed as main project, not as subproject)
if get_option('build_tests') and not meson.is_subproject()
  if pfr_use_modules
    pfr_module_usage = executable(
      'pfr_module_usage',
      files('modules/usage_sample.cpp'),
      dependencies: pfr_dep,
    )
    test('pfr_module_usage', pfr_module_usage)

    pfr_module_usage_mu = executable(
      'pfr_module_usage_mu',
      files('modules/usage_test_mu1.cpp', 'modules/usage_test_mu2.cpp'),
      dependencies: pfr_dep,
    )
    test('pfr_module_usage_mu', pfr_module_usage_mu)
  endif
endif
