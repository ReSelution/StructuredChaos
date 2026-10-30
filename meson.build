project(
  'StructuredChaos',
  'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++23', 'warning_level=3', 'buildtype=release', 'b_ndebug=if-release'],
  meson_version: '>= 1.1.0',
)

# Sanitizer builds are selected with the built-in option, for example
#   meson setup build-asan --buildtype=debug -Db_sanitize=address,undefined
#   meson setup build-tsan --buildtype=debug -Db_sanitize=thread
sc_sanitizers = get_option('b_sanitize')
sc_asan = 'address' in sc_sanitizers
sc_tsan = 'thread' in sc_sanitizers

# Sources are UTF-8 ("µs" in the stats). MSVC and clang-cl assume the local
# code page otherwise.
if meson.get_compiler('cpp').get_argument_syntax() == 'msvc'
  add_project_arguments('/utf-8', language: 'cpp')
endif

subdir('external')

# Config
conf_data = configuration_data()
conf_data.set_quoted('PROJECT_VERSION', meson.project_version())
if get_option('enable_chaos_stats')
  conf_data.set('CHAOS_STATS_ENABLED', 1)
endif

if get_option('dump_magpie')
  conf_data.set('DUMP_MAGPIE', 1)
endif

# Generated next to its template, so that it is found as "sc/config/config.h"
# through the same include directory as every other header.
subdir('src/sc/config')

inc_src = include_directories('src')

chaos_config_dep = declare_dependency(
  sources: config_h,
  include_directories: inc_src,
)

subdir('src')

if not meson.is_subproject()
  subdir('tests')
endif
