#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <new>
#include <thread>
#include <utility>
#include <vector>

#include "sc/memory/monotonic_resource.hpp"

#include "test_upstream.hpp"

namespace {

constexpr size_t MaxAlign = alignof(std::max_align_t);

bool isAligned(const void *ptr, size_t alignment) {
  return reinterpret_cast<uintptr_t>(ptr) % alignment == 0;
}

bool inside(const void *ptr, const std::byte *begin, size_t size) {
  auto addr = reinterpret_cast<uintptr_t>(ptr);
  auto start = reinterpret_cast<uintptr_t>(begin);
  return addr >= start && addr < start + size;
}

} // namespace

TEST_CASE("MonotonicResource: allocations are aligned and do not overlap",
          "[memory][monotonic]") {
  TrackingUpstream upstream;
  sc::MonotonicResource res(1024, &upstream);

  std::vector<std::pair<std::byte *, size_t>> chunks;
  for (size_t alignment : {size_t{1}, size_t{2}, size_t{4}, size_t{8}, size_t{16}}) {
    for (size_t bytes : {size_t{1}, size_t{3}, size_t{8}, size_t{24}}) {
      auto *ptr = static_cast<std::byte *>(res.allocate(bytes, alignment));
      REQUIRE(ptr != nullptr);
      CHECK(isAligned(ptr, alignment));
      CHECK(upstream.owns(ptr));
      std::memset(ptr, 0xAB, bytes);
      chunks.emplace_back(ptr, bytes);
    }
  }

  std::ranges::sort(chunks);
  for (size_t i = 1; i < chunks.size(); ++i) {
    CHECK(chunks[i - 1].first + chunks[i - 1].second <= chunks[i].first);
  }

  // Everything fit into the first block.
  CHECK(upstream.allocations == 1);
}

TEST_CASE("MonotonicResource: the first block is allocated up front",
          "[memory][monotonic]") {
  TrackingUpstream upstream;
  sc::MonotonicResource res(512, &upstream);

  CHECK(upstream.allocations == 1);
  CHECK(upstream.lastSize == 512);
  CHECK(upstream.lastAlignment == MaxAlign);
}

TEST_CASE("MonotonicResource: a full block is followed by a new one",
          "[memory][monotonic]") {
  TrackingUpstream upstream;
  sc::MonotonicResource res(256, &upstream);

  void *first = res.allocate(200, 8);
  void *second = res.allocate(200, 8);

  CHECK(first != second);
  CHECK(upstream.owns(first));
  CHECK(upstream.owns(second));
  CHECK(upstream.allocations == 2);
  CHECK(upstream.lastSize == 256);
}

TEST_CASE("MonotonicResource: a request larger than a block gets its own",
          "[memory][monotonic]") {
  TrackingUpstream upstream;
  sc::MonotonicResource res(256, &upstream);

  void *big = res.allocate(4096, 32);

  REQUIRE(big != nullptr);
  CHECK(isAligned(big, 32));
  CHECK(upstream.allocations == 2);
  CHECK(upstream.lastSize == 4096);
  CHECK(upstream.lastAlignment == 32);

  // The current block is still in use afterwards.
  void *small = res.allocate(16, 8);
  CHECK(upstream.owns(small));
  CHECK(upstream.allocations == 2);
}

TEST_CASE("MonotonicResource: an over-aligned request that almost fills a "
          "block terminates",
          "[memory][monotonic]") {
  TrackingUpstream upstream;
  sc::MonotonicResource res(1024, &upstream);

  // Fits a block by size, but not once it is padded to 64 bytes. The upstream
  // never returns a block that is aligned that well by accident.
  void *ptr = nullptr;
  REQUIRE_NOTHROW(ptr = res.allocate(1020, 64));

  REQUIRE(ptr != nullptr);
  CHECK(isAligned(ptr, 64));
  CHECK(upstream.owns(ptr));
  CHECK(upstream.allocations == 2);
  std::memset(ptr, 0xCD, 1020);
}

TEST_CASE("MonotonicResource: over-aligned requests share a block when the "
          "padding fits",
          "[memory][monotonic]") {
  TrackingUpstream upstream;
  sc::MonotonicResource res(1024, &upstream);

  for (int i = 0; i < 4; ++i) {
    void *ptr = res.allocate(64, 64);
    CHECK(isAligned(ptr, 64));
  }

  CHECK(upstream.allocations == 1);
}

TEST_CASE("MonotonicResource: a request too large to exist fails instead of "
          "wrapping around",
          "[memory][monotonic]") {
  TrackingUpstream upstream;
  constexpr size_t Huge = std::numeric_limits<size_t>::max() - 64;

  SECTION("with upstream blocks") {
    sc::MonotonicResource res(1024, &upstream);
    CHECK_THROWS_AS(res.allocate(Huge, 1), std::bad_alloc);
    CHECK_THROWS_AS(res.allocate(Huge, 16), std::bad_alloc);

    // The resource is still usable.
    CHECK(upstream.owns(res.allocate(32, 8)));
  }

  SECTION("with a caller buffer") {
    alignas(MaxAlign) std::array<std::byte, 256> buffer{};
    sc::MonotonicResource res(buffer.data(), buffer.size(), &upstream);
    CHECK_THROWS_AS(res.allocate(Huge, 1), std::bad_alloc);
    CHECK(inside(res.allocate(32, 8), buffer.data(), buffer.size()));
  }
}

TEST_CASE("MonotonicResource: a caller buffer is used before the upstream",
          "[memory][monotonic]") {
  TrackingUpstream upstream;
  alignas(MaxAlign) std::array<std::byte, 256> buffer{};
  sc::MonotonicResource res(buffer.data(), buffer.size(), &upstream);

  void *first = res.allocate(100, 8);
  void *second = res.allocate(100, 8);
  CHECK(inside(first, buffer.data(), buffer.size()));
  CHECK(inside(second, buffer.data(), buffer.size()));
  CHECK(upstream.allocations == 0);

  // Further blocks are twice the size of the buffer.
  void *third = res.allocate(100, 8);
  CHECK_FALSE(inside(third, buffer.data(), buffer.size()));
  CHECK(upstream.owns(third));
  CHECK(upstream.allocations == 1);
  CHECK(upstream.lastSize == 512);
}

TEST_CASE("MonotonicResource: deallocate does nothing", "[memory][monotonic]") {
  TrackingUpstream upstream;
  sc::MonotonicResource res(256, &upstream);

  void *first = res.allocate(32, 8);
  res.deallocate(first, 32, 8);
  void *second = res.allocate(32, 8);

  CHECK(first != second);
  CHECK(upstream.deallocations == 0);
}

TEST_CASE("MonotonicResource: release returns every block as it was requested",
          "[memory][monotonic]") {
  TrackingUpstream upstream;
  sc::MonotonicResource res(256, &upstream);

  (void)res.allocate(200, 8);
  (void)res.allocate(200, 8);
  (void)res.allocate(4096, 64);
  (void)res.allocate(250, 128);
  REQUIRE(upstream.liveBlocks() == 4);

  res.release();

  CHECK(upstream.liveBlocks() == 0);
  CHECK(upstream.deallocations == 4);
  CHECK(upstream.mismatches == 0);

  SECTION("and the resource can be used again") {
    void *ptr = res.allocate(64, 8);
    CHECK(upstream.owns(ptr));
    CHECK(upstream.liveBlocks() == 1);
  }

  SECTION("and a large request right after it needs only one block") {
    void *ptr = res.allocate(4096, 8);
    CHECK(upstream.owns(ptr));
    CHECK(upstream.liveBlocks() == 1);
  }
}

TEST_CASE("MonotonicResource: release goes back to the caller buffer",
          "[memory][monotonic]") {
  TrackingUpstream upstream;
  alignas(MaxAlign) std::array<std::byte, 256> buffer{};
  sc::MonotonicResource res(buffer.data(), buffer.size(), &upstream);

  void *first = res.allocate(64, 8);
  (void)res.allocate(1024, 8);
  REQUIRE(upstream.liveBlocks() == 1);

  res.release();

  CHECK(upstream.liveBlocks() == 0);
  CHECK(upstream.mismatches == 0);
  CHECK(res.allocate(64, 8) == first);
}

TEST_CASE("MonotonicResource: the destructor releases all blocks",
          "[memory][monotonic]") {
  TrackingUpstream upstream;
  {
    sc::MonotonicResource res(256, &upstream);
    (void)res.allocate(200, 8);
    (void)res.allocate(200, 8);
    (void)res.allocate(1000, 64);
    REQUIRE(upstream.liveBlocks() == 3);
  }

  CHECK(upstream.liveBlocks() == 0);
  CHECK(upstream.mismatches == 0);
}

#if defined(__x86_64__) && !defined(_MSC_VER)
TEST_CASE("MonotonicResource: the bump pointer is lock-free",
          "[memory][monotonic]") {
  // Needs -mcx16, which the build adds on x86-64.
  CHECK(sc::MonotonicResource::checkLockFree());
}
#endif

TEST_CASE("MonotonicResource: is only equal to itself", "[memory][monotonic]") {
  sc::MonotonicResource a(256);
  sc::MonotonicResource b(256);

  CHECK(a.is_equal(a));
  CHECK_FALSE(a.is_equal(b));
  CHECK_FALSE(a.is_equal(*std::pmr::get_default_resource()));
}

TEST_CASE("MonotonicResource: works as the resource of a pmr container",
          "[memory][monotonic]") {
  TrackingUpstream upstream;
  upstream.maxAllocations = 1024;
  sc::MonotonicResource res(256, &upstream);

  std::pmr::vector<int> values(&res);
  for (int i = 0; i < 1000; ++i) {
    values.push_back(i);
  }

  for (int i = 0; i < 1000; ++i) {
    REQUIRE(values[i] == i);
  }
  CHECK(upstream.owns(values.data()));
}

TEST_CASE("MonotonicResource: concurrent allocations do not overlap",
          "[memory][monotonic]") {
  constexpr size_t Threads = 8;
  constexpr size_t PerThread = 2000;
  constexpr size_t Bytes = 24;

  TrackingUpstream upstream;
  upstream.maxAllocations = 100000;
  sc::MonotonicResource res(4096, &upstream);

  std::vector<std::vector<std::byte *>> results(Threads);
  {
    std::vector<std::jthread> workers;
    for (size_t t = 0; t < Threads; ++t) {
      workers.emplace_back([&, t] {
        results[t].reserve(PerThread);
        for (size_t i = 0; i < PerThread; ++i) {
          auto *ptr = static_cast<std::byte *>(res.allocate(Bytes, 8));
          std::memset(ptr, static_cast<int>(t + 1), Bytes);
          results[t].push_back(ptr);
        }
      });
    }
  }

  // Every chunk still holds the pattern of the thread that got it.
  std::vector<std::byte *> all;
  size_t corrupted = 0;
  for (size_t t = 0; t < Threads; ++t) {
    for (std::byte *ptr : results[t]) {
      for (size_t i = 0; i < Bytes; ++i) {
        corrupted += ptr[i] != static_cast<std::byte>(t + 1);
      }
      all.push_back(ptr);
    }
  }
  CHECK(corrupted == 0);

  std::ranges::sort(all);
  size_t overlaps = 0;
  for (size_t i = 1; i < all.size(); ++i) {
    overlaps += all[i - 1] + Bytes > all[i];
  }
  CHECK(overlaps == 0);
  CHECK(all.size() == Threads * PerThread);
}
