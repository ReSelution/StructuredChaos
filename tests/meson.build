# Catch2 Dependency. Its own self-tests are a quarter of all objects and are
# not ours to run.
catch2_dep = dependency(
  'catch2-with-main',
  fallback: ['catch2', 'catch2_with_main_dep'],
  default_options: ['tests=false'],
)

# UBSan only prints by default, which would leave a failing run green.
test_env = environment()
test_env.set('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')

# Instrumented binaries are several times slower, and unoptimized ones leave
# little headroom for the contended threading tests on a small CI runner.
if sc_asan or sc_tsan
  test_timeout = 300
elif get_option('optimization') == '0'
  test_timeout = 120
else
  test_timeout = 30
endif

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

test('ECS Registry Tests', test_registry, env: test_env, timeout: test_timeout)
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
    'threading/test_future_group.cpp',
    'threading/test_threading.cpp',
  ],
  dependencies: [
    catch2_dep,
    sc_dep,
  ],
)

test('threading Tests', test_threading, env: test_env, timeout: test_timeout)

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

test('Hash Tests', test_hash, env: test_env, timeout: test_timeout)

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

test('Logger Tests', test_logger, env: test_env, timeout: test_timeout)

test_stats = executable(
  'test_stats',
  sources: [
    'stats/test_stats.cpp',
  ],
  # Tells the test what was configured, so it can check that the option
  # actually reaches the code.
  cpp_args: ['-DSC_TEST_EXPECT_STATS=' + get_option('enable_chaos_stats').to_int().to_string()],
  dependencies: [
    catch2_dep,
    sc_dep,
  ],
)

test('Stats Tests', test_stats, env: test_env, timeout: test_timeout)

test_magpie = executable(
  'test_magpie',
  sources: [
    'localizations/bench_magpie.cpp',
    'localizations/test_magpie.cpp',
  ],
  # Tells the test what was configured, so it can check that the option
  # actually reaches the code.
  cpp_args: ['-DSC_TEST_EXPECT_DUMP=' + get_option('dump_magpie').to_int().to_string()],
  dependencies: [
    catch2_dep,
    sc_dep,
  ],
)

test('Magpie Tests', test_magpie, env: test_env, timeout: test_timeout)
benchmark(
  'Magpie Benchmarks',
  test_magpie,
  args: ['[!benchmark]'],
  timeout: 300,
)

test_memory = executable(
  'test_memory',
  sources: [
    'memory/bench_heap.cpp',
    'memory/test_heap.cpp',
  ],
  dependencies: [
    catch2_dep,
    sc_dep,
  ],
)

test('Memory Tests', test_memory, env: test_env, timeout: test_timeout)
benchmark(
  'Memory Benchmarks',
  test_memory,
  args: ['[!benchmark]'],
  timeout: 300,
)

test_util = executable(
  'test_util',
  sources: [
    'util/test_fixed_string.cpp',
  ],
  dependencies: [
    catch2_dep,
    sc_dep,
  ],
)

test('Util Tests', test_util, env: test_env, timeout: test_timeout)
