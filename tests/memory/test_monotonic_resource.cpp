#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sc/memory/monotonic_resource.hpp"

#include "test_upstream.hpp"

namespace {

  constexpr size_t MaxAlign = alignof(std::max_align_t);

  bool isAligned(const void *ptr, size_t alignment) { return reinterpret_cast<uintptr_t>(ptr) % alignment == 0; }

  bool inside(const void *ptr, const std::byte *begin, size_t size) {
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    auto start = reinterpret_cast<uintptr_t>(begin);
    return addr >= start && addr < start + size;
  }

  // Writes its id into a log when it is destroyed.
  struct Tracked {
    std::vector<int> *log;
    int id;

    Tracked(std::vector<int> *log, int id) : log(log), id(id) {}
    ~Tracked() { log->push_back(id); }
  };

  struct Trivial {
    int a;
    double b;
  };

  struct alignas(64) Wide {
    std::byte data[64];
    ~Wide() {}
  };

} // namespace

TEST_CASE("MonotonicResource: allocations are aligned and do not overlap", "[memory][monotonic]") {
  TrackingUpstream upstream;
  sc::MonotonicResource res(1024, &upstream);

  std::vector<std::pair<std::byte *, size_t>> chunks;
  for (size_t alignment: {size_t{1}, size_t{2}, size_t{4}, size_t{8}, size_t{16}}) {
    for (size_t bytes: {size_t{1}, size_t{3}, size_t{8}, size_t{24}}) {
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

TEST_CASE("MonotonicResource: the first block is allocated up front", "[memory][monotonic]") {
  TrackingUpstream upstream;
  sc::MonotonicResource res(512, &upstream);

  CHECK(upstream.allocations == 1);
  CHECK(upstream.lastSize == 512);
  CHECK(upstream.lastAlignment == MaxAlign);
}

TEST_CASE("MonotonicResource: a full block is followed by a new one", "[memory][monotonic]") {
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

TEST_CASE("MonotonicResource: a request larger than a block gets its own", "[memory][monotonic]") {
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

TEST_CASE("MonotonicResource: a caller buffer is used before the upstream", "[memory][monotonic]") {
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

TEST_CASE("MonotonicResource: release returns every block as it was requested", "[memory][monotonic]") {
  TrackingUpstream upstream;
  sc::MonotonicResource res(256, &upstream);

  (void) res.allocate(200, 8);
  (void) res.allocate(200, 8);
  (void) res.allocate(4096, 64);
  (void) res.allocate(250, 128);
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

TEST_CASE("MonotonicResource: release goes back to the caller buffer", "[memory][monotonic]") {
  TrackingUpstream upstream;
  alignas(MaxAlign) std::array<std::byte, 256> buffer{};
  sc::MonotonicResource res(buffer.data(), buffer.size(), &upstream);

  void *first = res.allocate(64, 8);
  (void) res.allocate(1024, 8);
  REQUIRE(upstream.liveBlocks() == 1);

  res.release();

  CHECK(upstream.liveBlocks() == 0);
  CHECK(upstream.mismatches == 0);
  CHECK(res.allocate(64, 8) == first);
}

TEST_CASE("MonotonicResource: the destructor releases all blocks", "[memory][monotonic]") {
  TrackingUpstream upstream;
  {
    sc::MonotonicResource res(256, &upstream);
    (void) res.allocate(200, 8);
    (void) res.allocate(200, 8);
    (void) res.allocate(1000, 64);
    REQUIRE(upstream.liveBlocks() == 3);
  }

  CHECK(upstream.liveBlocks() == 0);
  CHECK(upstream.mismatches == 0);
}

#if defined(__x86_64__) && !defined(_MSC_VER)
TEST_CASE("MonotonicResource: the bump pointer is lock-free", "[memory][monotonic]") {
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

TEST_CASE("MonotonicResource: works as the resource of a pmr container", "[memory][monotonic]") {
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

TEST_CASE("MonotonicResource: concurrent allocations do not overlap", "[memory][monotonic]") {
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
    for (std::byte *ptr: results[t]) {
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

TEST_CASE("MonotonicResource: make constructs the object in the resource", "[memory][monotonic]") {
  sc::MonotonicResource res(1024);

  auto *trivial = res.make<Trivial>(7, 2.5);
  REQUIRE(trivial != nullptr);
  CHECK(trivial->a == 7);
  CHECK(trivial->b == 2.5);
  CHECK(isAligned(trivial, alignof(Trivial)));

  auto *text = res.make<std::string>(40, 'x');
  CHECK(*text == std::string(40, 'x'));

  auto *wide = res.make<Wide>();
  CHECK(isAligned(wide, 64));
}

TEST_CASE("MonotonicResource: reset destroys the objects in reverse order", "[memory][monotonic]") {
  std::vector<int> log;
  sc::MonotonicResource res(1024);

  res.make<Tracked>(&log, 1);
  res.make<Trivial>(0, 0.0);
  res.make<Tracked>(&log, 2);
  res.make<Tracked>(&log, 3);
  REQUIRE(log.empty());

  res.reset();
  CHECK(log == std::vector<int>{3, 2, 1});

  // Nothing is destroyed a second time.
  res.reset();
  CHECK(log.size() == 3);
}

TEST_CASE("MonotonicResource: the destructor destroys the objects", "[memory][monotonic]") {
  std::vector<int> log;
  {
    sc::MonotonicResource res(1024);
    res.make<Tracked>(&log, 1);
    res.make<Tracked>(&log, 2);
    REQUIRE(log.empty());
  }

  CHECK(log == std::vector<int>{2, 1});
}

TEST_CASE("MonotonicResource: an object whose constructor throws is not destroyed", "[memory][monotonic]") {
  struct Throwing {
    std::vector<int> *log;

    explicit Throwing(std::vector<int> *log) : log(log) { throw 42; }
    ~Throwing() { log->push_back(-1); }
  };

  std::vector<int> log;
  sc::MonotonicResource res(1024);

  res.make<Tracked>(&log, 1);
  CHECK_THROWS_AS(res.make<Throwing>(&log), int);
  res.make<Tracked>(&log, 2);

  res.reset();
  CHECK(log == std::vector<int>{2, 1});
}

TEST_CASE("MonotonicResource: reset makes the memory available again", "[memory][monotonic]") {
  sc::MonotonicResource res(1024);

  void *first = res.allocate(64, 8);
  (void) res.allocate(4096, 8);
  res.reset();

  CHECK(res.allocate(64, 8) == first);
}

TEST_CASE("MonotonicResource: grows beyond its initial size", "[memory][monotonic]") {
  std::vector<int> log;
  sc::MonotonicResource res(128);

  std::vector<std::span<uint8_t>> spans;
  for (int i = 0; i < 100; ++i) {
    auto span = res.allocateSpan<uint8_t>(100);
    std::memset(span.data(), i, span.size());
    spans.push_back(span);
    res.make<Tracked>(&log, i);
  }

  size_t corrupted = 0;
  for (int i = 0; i < 100; ++i) {
    for (uint8_t byte: spans[i]) {
      corrupted += byte != static_cast<uint8_t>(i);
    }
  }
  CHECK(corrupted == 0);

  res.reset();
  CHECK(log.size() == 100);
}

TEST_CASE("MonotonicResource: allocateSpan", "[memory][monotonic]") {
  sc::MonotonicResource res(1024);

  SECTION("an empty span for a count of zero") {
    auto span = res.allocateSpan<int>(0);
    CHECK(span.empty());
    CHECK(span.data() == nullptr);
  }

  SECTION("holds count elements with the alignment of the type") {
    auto span = res.allocateSpan<double>(10);
    REQUIRE(span.size() == 10);
    CHECK(isAligned(span.data(), alignof(double)));
    for (size_t i = 0; i < span.size(); ++i) {
      span[i] = static_cast<double>(i);
    }
    CHECK(span[9] == 9.0);
  }

  SECTION("bytes by default") {
    auto span = res.allocateSpan(5);
    CHECK(span.size_bytes() == 5);
  }

  SECTION("takes a stricter alignment") {
    auto span = res.allocateSpan<uint8_t>(10, 64);
    CHECK(isAligned(span.data(), 64));
  }
}

TEST_CASE("MonotonicResource: feeds pmr containers", "[memory][monotonic]") {
  sc::MonotonicResource res(1024);

  std::pmr::vector<int> values(&res);
  for (int i = 0; i < 500; ++i) {
    values.push_back(i);
  }
  std::pmr::string text("a string that is too long for the small buffer", &res);

  CHECK(values.back() == 499);
  CHECK(text.size() == 46);
}

TEST_CASE("MonotonicResource: make is safe from several threads", "[memory][monotonic]") {
  constexpr int Threads = 8;
  constexpr int PerThread = 500;

  sc::MonotonicResource res(4096);
  std::vector<std::vector<std::string *>> results(Threads);
  {
    std::vector<std::jthread> workers;
    for (int t = 0; t < Threads; ++t) {
      workers.emplace_back([&, t] {
        for (int i = 0; i < PerThread; ++i) {
          results[t].push_back(res.make<std::string>(std::to_string(t * PerThread + i)));
        }
      });
    }
  }

  size_t wrong = 0;
  for (int t = 0; t < Threads; ++t) {
    for (int i = 0; i < PerThread; ++i) {
      wrong += *results[t][i] != std::to_string(t * PerThread + i);
    }
  }
  CHECK(wrong == 0);
}

TEST_CASE("MonotonicResource: makePmr puts the resource in as the allocator", "[memory][monotonic]") {
  TrackingUpstream upstream;
  sc::MonotonicResource res(256, &upstream);

  SECTION("the container and its elements live in the resource") {
    auto *values = res.makePmr<std::pmr::vector<int>>();
    CHECK(values->get_allocator().resource() == &res);
    for (int i = 0; i < 500; ++i) {
      values->push_back(i);
    }
    CHECK(values->back() == 499);
    CHECK(upstream.owns(values));
    CHECK(upstream.owns(values->data()));
  }

  SECTION("passes the arguments on") {
    auto *text = res.makePmr<std::pmr::string>("a string that is too long for the small buffer");
    auto *values = res.makePmr<std::pmr::vector<int>>(100, 3);
    CHECK(text->size() == 46);
    CHECK(values->size() == 100);
    CHECK(values->back() == 3);
  }

  SECTION("nested containers get the resource too") {
    auto *texts = res.makePmr<std::pmr::vector<std::pmr::string>>();
    texts->emplace_back("a string that is too long for the small buffer");
    CHECK(texts->back().get_allocator().resource() == &res);
  }

  SECTION("elements that own something else are destroyed") {
    std::vector<int> log;
    auto *objects = res.makePmr<std::pmr::vector<Tracked>>();
    objects->reserve(2);
    objects->emplace_back(&log, 1);
    objects->emplace_back(&log, 2);

    res.reset();
    CHECK(log == std::vector<int>{1, 2});
  }
}

TEST_CASE("MonotonicResource: reset keeps the first block", "[memory][monotonic]") {
  TrackingUpstream upstream;
  sc::MonotonicResource res(256, &upstream);

  void *first = res.allocate(64, 8);
  (void) res.allocate(200, 8);
  (void) res.allocate(4096, 64);
  REQUIRE(upstream.liveBlocks() == 3);

  res.reset();

  CHECK(upstream.liveBlocks() == 1);
  CHECK(upstream.mismatches == 0);
  CHECK(res.allocate(64, 8) == first);
  CHECK(upstream.allocations == 3);
}
