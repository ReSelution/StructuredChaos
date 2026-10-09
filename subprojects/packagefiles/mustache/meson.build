project(
  'mustache',
  'cpp',
  version: '0.2',
  license: 'MIT',
  default_options: ['cpp_std=c++17', 'warning_level=0'],
  meson_version: '>=1.1.0',
)

# What generate_export_header of CMake writes for a static library.
export_conf = configuration_data()
export_conf.set('MUSTACHE_EXPORT', '')
export_conf.set('MUSTACHE_NO_EXPORT', '')
configure_file(output: 'mustache_export.h', configuration: export_conf)

mustache_inc = include_directories('src', '.', is_system: true)

mustache_sources = files(
  'src/mustache/c_api.cpp',
  'src/mustache/ecs/archetype.cpp',
  'src/mustache/ecs/archetype_operation_helper.cpp',
  'src/mustache/ecs/base_component_data_storage.cpp',
  'src/mustache/ecs/base_job.cpp',
  'src/mustache/ecs/component_factory.cpp',
  'src/mustache/ecs/component_info.cpp',
  'src/mustache/ecs/component_mask.cpp',
  'src/mustache/ecs/component_version_storage.cpp',
  'src/mustache/ecs/default_component_data_storage.cpp',
  'src/mustache/ecs/entity.cpp',
  'src/mustache/ecs/entity_builder.cpp',
  'src/mustache/ecs/entity_group.cpp',
  'src/mustache/ecs/entity_manager.cpp',
  'src/mustache/ecs/event_manager.cpp',
  'src/mustache/ecs/new_component_data_storage.cpp',
  'src/mustache/ecs/non_template_job.cpp',
  'src/mustache/ecs/stable_latency_component_data_storage.cpp',
  'src/mustache/ecs/system.cpp',
  'src/mustache/ecs/system_manager.cpp',
  'src/mustache/ecs/temporal_storage.cpp',
  'src/mustache/ecs/world.cpp',
  'src/mustache/ecs/world_filter.cpp',
  'src/mustache/ecs/world_storage.cpp',
  'src/mustache/utils/benchmark.cpp',
  'src/mustache/utils/dispatch.cpp',
  'src/mustache/utils/logger.cpp',
  'src/mustache/utils/memory_manager.cpp',
  'src/mustache/utils/timer.cpp',
  'src/mustache/utils/type_info.cpp',
)

# The profiler integration stays off, as in the default CMake build.
mustache_lib = static_library(
  'mustache',
  mustache_sources,
  include_directories: mustache_inc,
  dependencies: [dependency('threads')],
)

mustache_dep = declare_dependency(
  link_with: mustache_lib,
  include_directories: mustache_inc,
  dependencies: [dependency('threads')],
  version: meson.project_version(),
)

meson.override_dependency('mustache', mustache_dep)
