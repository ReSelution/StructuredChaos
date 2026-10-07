#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <thread>
#include <vector>

#include <mimalloc.h>

#include "sc/memory/heap.hpp"

// Hidden by the [!benchmark] tag, so that a plain test run stays fast. Run with
//   meson test --benchmark -C <builddir>
// or directly with
//   test_memory "[!benchmark]"
// Use an optimized build. Every benchmark is one scope: get an allocator, make
// a number of allocations, give everything back at once. The entries are
//   Heap             a fresh sc::Heap per scope
//   Heap, reset      one sc::Heap for all scopes, emptied with reset()
//   mi_heap          the bare mimalloc calls, to show what the wrapper costs
//   mi_malloc        plain mimalloc, every block freed on its own
//   pmr monotonic    std::pmr::monotonic_buffer_resource, a bump allocator
//                    that is not thread safe, as the lower bound

namespace {

constexpr size_t FIRST_BLOCK = 16 * 1024;

// Writes to the block, so that a block that was never mapped costs something.
inline uintptr_t touch(void *ptr) {
  *static_cast<unsigned char *>(ptr) = 1;
  return reinterpret_cast<uintptr_t>(ptr);
}

uintptr_t fill(sc::Heap &heap, size_t count, size_t bytes) {
  uintptr_t sum = 0;
  for (size_t i = 0; i < count; ++i) {
    sum += touch(heap.allocate(bytes, 8));
  }
  return sum;
}

uintptr_t heap_scope(size_t count, size_t bytes) {
  sc::Heap heap;
  return fill(heap, count, bytes);
}

uintptr_t reset_scope(sc::Heap &heap, size_t count, size_t bytes) {
  uintptr_t sum = fill(heap, count, bytes);
  heap.reset();
  return sum;
}

uintptr_t mi_heap_scope(size_t count, size_t bytes) {
  mi_heap_t *heap = mi_heap_new();
  uintptr_t sum = 0;
  for (size_t i = 0; i < count; ++i) {
    sum += touch(mi_heap_malloc(heap, bytes));
  }
  mi_heap_destroy(heap);
  return sum;
}

uintptr_t malloc_scope(std::vector<void *> &blocks, size_t bytes) {
  uintptr_t sum = 0;
  for (void *&block : blocks) {
    block = mi_malloc(bytes);
    sum += touch(block);
  }
  for (void *block : blocks) {
    mi_free(block);
  }
  return sum;
}

uintptr_t pmr_scope(size_t count, size_t bytes) {
  std::pmr::monotonic_buffer_resource resource(FIRST_BLOCK);
  uintptr_t sum = 0;
  for (size_t i = 0; i < count; ++i) {
    sum += touch(resource.allocate(bytes, 8));
  }
  return sum;
}

void run_scopes(size_t count, size_t bytes) {
  const std::string label =
      std::to_string(count) + " x " + std::to_string(bytes) + " B";
  std::vector<void *> blocks(count);
  sc::Heap reused;

  BENCHMARK("Heap:          " + label) { return heap_scope(count, bytes); };
  BENCHMARK("Heap, reset:   " + label) {
    return reset_scope(reused, count, bytes);
  };
  BENCHMARK("mi_heap:       " + label) { return mi_heap_scope(count, bytes); };
  BENCHMARK("mi_malloc:     " + label) { return malloc_scope(blocks, bytes); };
  BENCHMARK("pmr monotonic: " + label) { return pmr_scope(count, bytes); };
}

// Runs work on every thread at once and waits for all of them.
template <typename Work> void on_threads(size_t threads, Work work) {
  std::vector<std::jthread> workers;
  workers.reserve(threads);
  for (size_t t = 0; t < threads; ++t) {
    workers.emplace_back(work);
  }
}

} // namespace

TEST_CASE("Benchmark: one scope of small allocations", "[!benchmark][memory]") {
  run_scopes(100, 64);
  run_scopes(10'000, 64);
  run_scopes(1'000, 1024);
}

TEST_CASE("Benchmark: one scope of objects", "[!benchmark][memory]") {
  constexpr size_t COUNT = 10'000;

  struct Plain {
    uint64_t values[8];
  };
  // The same size, but the heap has to remember it for its destructor.
  struct Destroyed {
    uint64_t values[8];

    ~Destroyed() { values[0] = 0; }
  };
  struct alignas(64) WideDestroyed {
    uint64_t values[8];

    ~WideDestroyed() { values[0] = 0; }
  };

  BENCHMARK("make: 10000 x 64 B without destructor") {
    sc::Heap heap;
    uintptr_t sum = 0;
    for (size_t i = 0; i < COUNT; ++i) {
      sum += touch(heap.make<Plain>());
    }
    return sum;
  };

  BENCHMARK("make: 10000 x 64 B with destructor") {
    sc::Heap heap;
    uintptr_t sum = 0;
    for (size_t i = 0; i < COUNT; ++i) {
      sum += touch(heap.make<Destroyed>());
    }
    return sum;
  };

  BENCHMARK("make: 10000 x 64 B with destructor, aligned to 64") {
    sc::Heap heap;
    uintptr_t sum = 0;
    for (size_t i = 0; i < COUNT; ++i) {
      sum += touch(heap.make<WideDestroyed>());
    }
    return sum;
  };
}

TEST_CASE("Benchmark: one scope of pmr containers", "[!benchmark][memory]") {
  constexpr size_t COUNT = 1'000;

  // Every vector gets a buffer of its own, which its destructor would have
  // to give back one by one.
  BENCHMARK("makePmr: 1000 pmr vectors") {
    sc::Heap heap;
    uintptr_t sum = 0;
    for (size_t i = 0; i < COUNT; ++i) {
      auto *values = heap.makePmr<std::pmr::vector<uint64_t>>();
      values->resize(16);
      sum += touch(values->data());
    }
    return sum;
  };

  BENCHMARK("make: 1000 pmr vectors in the heap") {
    sc::Heap heap;
    uintptr_t sum = 0;
    for (size_t i = 0; i < COUNT; ++i) {
      auto *values = heap.make<std::pmr::vector<uint64_t>>(&heap);
      values->resize(16);
      sum += touch(values->data());
    }
    return sum;
  };

  BENCHMARK("make: 1000 pmr vectors in the default resource") {
    sc::Heap heap;
    uintptr_t sum = 0;
    for (size_t i = 0; i < COUNT; ++i) {
      auto *values = heap.make<std::pmr::vector<uint64_t>>();
      values->resize(16);
      sum += touch(values->data());
    }
    return sum;
  };
}

TEST_CASE("Benchmark: one scope of large blocks", "[!benchmark][memory]") {
  run_scopes(16, 1024 * 1024);
}

TEST_CASE("Benchmark: every thread with its own scopes",
          "[!benchmark][memory]") {
  constexpr size_t THREADS = 8;
  constexpr size_t SCOPES = 200;
  constexpr size_t COUNT = 1'000;
  constexpr size_t BYTES = 64;

  BENCHMARK("Heap: 8 threads x 200 scopes") {
    on_threads(THREADS, [] {
      uintptr_t sum = 0;
      for (size_t i = 0; i < SCOPES; ++i) {
        sum += heap_scope(COUNT, BYTES);
      }
      return sum;
    });
  };
}

TEST_CASE("Benchmark: all threads share one allocator",
          "[!benchmark][memory]") {
  constexpr size_t THREADS = 8;
  constexpr size_t COUNT = 20'000;
  constexpr size_t BYTES = 64;

  BENCHMARK("Heap: 8 threads x 20000 allocations") {
    sc::Heap heap;
    on_threads(THREADS, [&heap] { return fill(heap, COUNT, BYTES); });
  };
}
