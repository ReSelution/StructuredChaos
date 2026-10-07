project(
  'StructuredChaos',
  'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++23'],
  meson_version: '>= 1.1.0',
)
subdir('external')
#Config
conf_data = configuration_data()

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

chaos_config_dep = declare_dependency(sources: config_h, include_directories: inc_src)

subdir('src')

if host_machine.system() != 'windows'
  run_command(
    'ln',
    '-sf', meson.project_build_root() / 'compile_commands.json',
    meson.project_source_root() / 'compile_commands.json',
    check: false,
  )
endif

subdir('tests')
