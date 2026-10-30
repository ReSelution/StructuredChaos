# Dependency
spdlog_dep = dependency(
  'spdlog',
  fallback: ['spdlog', 'spdlog_dep'],
  default_options: ['default_library=static'],
)
unordered_dense_dep = dependency(
  'unordered_dense',
  fallback: ['unordered_dense', 'unordered_dense_dep'],
)

mimalloc_options = [
  'mi_build_shared=false',
  'mi_build_object=false',
  'mi_build_tests=false',

  # mimalloc 3.5.4 aborts in its internal debug checks ("corrupted meta-data
  # in thread-free list") when a heap is destroyed that still has blocks
  # freed from another thread. sc::Heap does exactly that on purpose: pmr
  # containers give their memory back right away. Use an address sanitizer
  # build to check our own code instead.
  'mi_debug=off',

  # mimalloc asks for transparent huge pages by default. Their first touch
  # zeroes 2 MiB at once, which costs more than it gains for many short-lived
  # allocations: the asset crawl of SAA took about 65 ms with them and 51 ms
  # without. MI_NO_THP only removes that request. mi_allow_thp=off would also
  # disable huge pages for the whole process, including memory that the
  # application asks them for itself. On a system whose THP mode is "always"
  # the memory of mimalloc still gets huge pages.
  'mi_extra_cppdefs=MI_NO_THP',
]

# Sanitizer runtimes interpose malloc themselves, so mimalloc must not override
# it. The heaps are still used directly and report their blocks to the runtime.
if sc_asan or sc_tsan
  mimalloc_options += 'mi_override=false'
endif
if sc_asan
  mimalloc_options += 'mi_track=asan'
endif
if sc_tsan
  mimalloc_options += 'mi_debug_tsan=true'
endif

# mimalloc replaces malloc and new for the whole program (except under the
# sanitizers above), so its fixes concern every allocation: 3.5.3 fixes the
# first new on a thread failing on some platforms (#1398). sc::Heap
# (sc/memory/heap.hpp) also needs the first-class heaps of v3 that allocate
# from any thread (since 3.2.6) and the fixes of mi_heap_destroy up to 3.4.5.
mimalloc_dep = dependency(
  'mimalloc',
  version: '>=3.5.3',
  fallback: ['mimalloc', 'mimalloc_static_dep'],
  default_options: mimalloc_options,
)

simdutf_dep = dependency(
  'simdutf',
  fallback: ['simdutf', 'simdutf_dep'],
  default_options: ['default_library=static'],
)
rapidHash_dep = dependency('rapidHash', fallback: ['rapidHash', 'rapidHash_dep'])
# Its tests need Boost.
atomic_queue_dep = dependency(
  'atomic_queue',
  fallback: ['atomic_queue', 'atomic_queue_dep'],
  default_options: ['tests=false'],
)

# sc::io::FileReader reads through io_uring on Linux; elsewhere it reads
# synchronously and needs nothing.
if host_machine.system() == 'linux'
  liburing_dep = dependency('liburing', fallback: ['liburing', 'liburing_dep'])
else
  liburing_dep = dependency('', required: false)
endif

# Needed by the public headers, so every consumer gets them as well.
sc_public_deps = [
  spdlog_dep,
  unordered_dense_dep,
  mimalloc_dep,
  rapidHash_dep,
  atomic_queue_dep,
]

# Only used inside the library's own sources.
sc_private_deps = [
  liburing_dep,
  simdutf_dep,
]
