# Catch2 Dependency
catch2_dep = dependency('catch2-with-main', fallback: ['catch2', 'catch2_with_main_dep'])

# Registry Test Executable
test_registry = executable(
  'test_registry',
  sources: [
    'ecs/test_registry.cpp',
  ],
  dependencies: [
    catch2_dep,
    sc_dep,
  ],
)

test('ECS Registry Tests', test_registry)

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
