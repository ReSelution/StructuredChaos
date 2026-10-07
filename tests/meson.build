# Catch2 Dependency
catch2_dep = dependency('catch2-with-main', fallback: ['catch2', 'catch2_with_main_dep'])

# Registry Test Executable
test_registry = executable(
  'test_registry',
  sources: [
    'ecs/bench_registry.cpp',
    'ecs/test_component_lock.cpp',
    'ecs/test_destroy.cpp',
    'ecs/test_groups.cpp',
    'ecs/test_registry.cpp',
  ],
  dependencies: [
    catch2_dep,
    sc_dep,
  ],
)

test('ECS Registry Tests', test_registry)
benchmark(
  'ECS Registry Benchmarks',
  test_registry,
  args: ['[!benchmark]'],
  timeout: 300,
)

test_threading = executable(
  'test_threading',
  sources: [
    'threading/test_atomic_queue.cpp',
    'threading/test_threading.cpp',
  ],
  dependencies: [
    catch2_dep,
    sc_dep,
  ],
)

test('threading Tests', test_threading)

test_hash = executable(
  'test_hash',
  sources: [
    'hash/test_hash.cpp',
  ],
  dependencies: [
    catch2_dep,
    sc_dep,
  ],
)

test('Hash Tests', test_hash)

test_logger = executable(
  'test_logger',
  sources: [
    'logger/test_logger.cpp',
  ],
  dependencies: [
    catch2_dep,
    sc_dep,
  ],
)

test('Logger Tests', test_logger)
