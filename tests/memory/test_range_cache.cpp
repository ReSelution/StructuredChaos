#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

#include "sc/memory/range_cache.hpp"

namespace {

  using sc::memory::RangeCache;

  constexpr size_t SIZE = size_t{1} << 20;

} // namespace

TEST_CASE("Range Cache", "[memory][virtual]") {
  SECTION("A Range Given Back Comes Back With Its Memory") {
    RangeCache cache;
    auto *const first = static_cast<unsigned char *>(cache.reserve(SIZE));
    sc::memory::commit(first, SIZE);
    std::memset(first, 0xab, SIZE);
    cache.release(first, SIZE);
    REQUIRE(cache.ranges() == 1);
    REQUIRE(cache.bytes() == SIZE);

    // Only for the same size.
    void *const other = cache.reserve(2 * SIZE);
    REQUIRE(other != first);
    REQUIRE(cache.ranges() == 1);

    auto *const again = static_cast<unsigned char *>(cache.reserve(SIZE));
    REQUIRE(again == first);
    REQUIRE(cache.ranges() == 0);
    REQUIRE(cache.bytes() == 0);
    // What the OS did not take is still there, the rest is zero; and it is
    // usable either way.
    size_t wrong = 0;
    for (size_t i = 0; i < SIZE; ++i) {
      wrong += static_cast<size_t>(again[i] != 0xab && again[i] != 0);
    }
    REQUIRE(wrong == 0);
    std::memset(again, 0xcd, SIZE);
    REQUIRE(again[SIZE - 1] == 0xcd);
    cache.release(again, SIZE);
    cache.release(other, 2 * SIZE);
  }

  SECTION("The Last One Given Back Comes Back First") {
    RangeCache cache;
    void *const a = cache.reserve(SIZE);
    void *const b = cache.reserve(SIZE);
    void *const c = cache.reserve(2 * SIZE);
    cache.release(a, SIZE);
    cache.release(c, 2 * SIZE);
    cache.release(b, SIZE);
    REQUIRE(cache.reserve(SIZE) == b);
    REQUIRE(cache.reserve(SIZE) == a);
    REQUIRE(cache.ranges() == 1);
    REQUIRE(cache.reserve(2 * SIZE) == c);
    cache.release(a, SIZE);
    cache.release(b, SIZE);
    cache.release(c, 2 * SIZE);
  }

  SECTION("No More Than The Limit Is Kept") {
    RangeCache cache{3 * SIZE};
    std::vector<void *> ranges;
    for (int i = 0; i < 4; ++i) {
      ranges.push_back(cache.reserve(SIZE));
    }
    for (void *const range: ranges) {
      cache.release(range, SIZE);
    }
    // The first one made room for the last.
    REQUIRE(cache.ranges() == 3);
    REQUIRE(cache.bytes() == 3 * SIZE);

    // A larger one takes the place of the two kept longest.
    void *const large = cache.reserve(2 * SIZE);
    cache.release(large, 2 * SIZE);
    REQUIRE(cache.ranges() == 2);
    REQUIRE(cache.bytes() == 3 * SIZE);
    REQUIRE(cache.reserve(SIZE) == ranges[3]);
    cache.release(ranges[3], SIZE);

    // One larger than the limit is not kept, and takes nothing's place.
    void *const huge = cache.reserve(4 * SIZE);
    cache.release(huge, 4 * SIZE);
    REQUIRE(cache.ranges() == 2);

    // A lower limit gives back the ones kept longest.
    cache.limit(SIZE);
    REQUIRE(cache.limit() == SIZE);
    REQUIRE(cache.ranges() == 1);
    REQUIRE(cache.reserve(SIZE) == ranges[3]);
    cache.release(ranges[3], SIZE);

    cache.trim();
    REQUIRE(cache.ranges() == 0);
    REQUIRE(cache.bytes() == 0);
  }

  SECTION("No More Than MAX_RANGES Are Kept, The Last Ones") {
    RangeCache cache;
    std::vector<void *> ranges;
    for (size_t i = 0; i < RangeCache::MAX_RANGES + 3; ++i) {
      ranges.push_back(cache.reserve(sc::memory::OS_PAGE_SIZE));
    }
    for (void *const range: ranges) {
      cache.release(range, sc::memory::OS_PAGE_SIZE);
    }
    REQUIRE(cache.ranges() == RangeCache::MAX_RANGES);
    REQUIRE(cache.reserve(sc::memory::OS_PAGE_SIZE) == ranges.back());
    cache.release(ranges.back(), sc::memory::OS_PAGE_SIZE);
  }

  SECTION("Threads Take And Give Back") {
    RangeCache cache{8 * SIZE};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
      threads.emplace_back([&cache, t] {
        for (int round = 0; round < 200; ++round) {
          const size_t bytes = (round + t) % 2 == 0 ? SIZE : 2 * SIZE;
          auto *const range = static_cast<unsigned char *>(cache.reserve(bytes));
          sc::memory::commit(range, sc::memory::OS_PAGE_SIZE);
          range[0] = static_cast<unsigned char>(t);
          cache.release(range, bytes);
        }
      });
    }
    for (std::thread &thread: threads) {
      thread.join();
    }
    REQUIRE(cache.bytes() <= 8 * SIZE);
  }

  SECTION("The One Of The Process") {
    REQUIRE(&sc::memory::rangeCache() == &sc::memory::rangeCache());
    REQUIRE(sc::memory::rangeCache().limit() == RangeCache::DEFAULT_LIMIT);
  }
}
