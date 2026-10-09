# sc::ecs next to other ECS libraries: one executable per library, all of them
# doing the same work, see common.hpp. scripts/ecs_compare.py builds them in
# a build directory of its own, runs them and puts the results side by side.
# The target does the same for this build directory:
#   meson compile -C <builddir> ecs_compare

flecs_dep = dependency(
  'flecs',
  fallback: ['flecs', 'flecs_dep'],
  default_options: ['default_library=static', 'warning_level=0'],
).as_system()
gaia_ecs_dep = dependency('gaia-ecs', fallback: ['gaia-ecs', 'gaia_ecs_dep'])
ginseng_dep = dependency('ginseng', fallback: ['ginseng', 'ginseng_dep'])
# Built with the standard of the benchmarks: its headers define inline
# variables that are constants in one standard and initialized at startup in
# another, and the two must not meet in one executable.
mustache_dep = dependency(
  'mustache',
  fallback: ['mustache', 'mustache_dep'],
  default_options: ['cpp_std=' + get_option('cpp_std')],
)

ecs_compare_libraries = {
  'sc': [sc_dep],
  # The prototype of an ECS of our own, see src/sc/ecs2.
  'sc2': [sc_dep],
  'entt': [entt_dep],
  'flecs': [flecs_dep],
  'gaia': [gaia_ecs_dep],
  'ginseng': [ginseng_dep],
  'mustache': [mustache_dep],
}

ecs_compare_executables = []
foreach name, dependencies : ecs_compare_libraries
  ecs_compare_executables += executable(
    'ecs_compare_' + name,
    sources: ['bench_' + name + '.cpp'],
    # Every library allocates with mimalloc, as sc::ecs does, so that the
    # allocator is not what is compared.
    dependencies: [catch2_dep, mimalloc_dep] + dependencies,
  )
endforeach

run_target(
  'ecs_compare',
  command: [
    find_program('python3'),
    files('../../scripts/ecs_compare.py'),
    '--build-dir', meson.project_build_root(),
    # The target has built them, and ninja is not to be run from inside ninja.
    '--no-build',
  ],
  depends: ecs_compare_executables,
)
