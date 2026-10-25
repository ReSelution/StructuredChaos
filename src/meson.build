sc_sources = files(
  'sc/io/file_reader.cpp',
  'sc/localizations/magpie_translator.cpp',
  'sc/logger/logger.cpp',
  'sc/memory/arena_pool.cpp',
  'sc/memory/monotonic_resource.cpp',
  'sc/memory/virtual_memory.cpp',
  'sc/stats/core.cpp',
  'sc/threading/background.cpp',
  'sc/threading/backend.cpp',
  'sc/threading/ring_queue.cpp',
  'sc/threading/threading.cpp',
)

sc_lib = static_library(
  'structured_chaos',
  sc_sources,
  dependencies: [chaos_config_dep, sc_public_deps, sc_private_deps],
)

# A static mimalloc cannot override new and delete on Windows, so the override
# is compiled into every executable that uses sc_dep. Consumers with their own
# operator new turn it off with -Dmimalloc_new_delete=false.
sc_dep_sources = []
if host_machine.system() == 'windows' and get_option('mimalloc_new_delete')
  sc_dep_sources += files('sc/memory/mi_new_delete.cpp')
endif

# Reusable dependency object for consumers
sc_dep = declare_dependency(
  sources: sc_dep_sources,
  link_with: sc_lib,
  dependencies: [chaos_config_dep, sc_public_deps, sc_bundled_deps],
)
