sc_internal_inc = include_directories('sc')
sc_sources = files(
  'sc/ecs/entity.cpp',
  'sc/ecs/registry.cpp',
  'sc/localizations/magpie_translator.cpp',
  'sc/stats/core.cpp',
  'sc/threading/backend.cpp',
  'sc/threading/threading.cpp',
)

sc_lib = static_library(
  'structured_chaos',
  sc_sources,
  include_directories: sc_internal_inc,
  dependencies: [chaos_config_dep, all_deps],
)

# Reusable dependency object for consumers
sc_dep_sources = []
if host_machine.system() == 'windows'
  sc_dep_sources += files('sc/memory/mi_new_delete.cpp')
endif

sc_dep = declare_dependency(
  sources: sc_dep_sources,
  link_with: sc_lib,
  dependencies: [chaos_config_dep, all_deps],
)
