#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "sc/memory/arena_pool.hpp"
#include "sc/memory/monotonic_resource.hpp"

namespace {

  // Writes its id into a log when it is destroyed.
  struct Tracked {
    std::vector<int> *log;
    int id;

    Tracked(std::vector<int> *log, int id) : log(log), id(id) {}
    ~Tracked() { log->push_back(id); }
  };

} // namespace

TEST_CASE("ArenaPool: hands out and takes back resources", "[memory][pool]") {
  sc::ArenaPool pool(2, 2048);
  CHECK(pool.arenaSize() == 2048);

  SECTION("acquire always returns an arena") {
    std::vector<std::unique_ptr<sc::MonotonicResource>> arenas;
    // More than the pool was filled with.
    for (int i = 0; i < 8; ++i) {
      arenas.push_back(pool.acquire());
      REQUIRE(arenas.back() != nullptr);
      CHECK(arenas.back()->allocate(64, 8) != nullptr);
    }
    for (auto &arena: arenas) {
      pool.release(std::move(arena));
    }
  }

  SECTION("release takes the arena and destroys its objects") {
    std::vector<int> log;
    auto arena = pool.acquire();
    arena->make<Tracked>(&log, 1);

    pool.release(std::move(arena));

    CHECK(log == std::vector<int>{1});
  }

  SECTION("a released arena is handed out again, empty") {
    auto arena = pool.acquire();
    sc::MonotonicResource *address = arena.get();
    void *first = arena->allocate(64, 8);
    pool.release(std::move(arena));

    auto again = pool.acquire();
    CHECK(again.get() == address);
    CHECK(again->allocate(64, 8) == first);
    pool.release(std::move(again));
  }

  SECTION("more arenas than the pool keeps can be released") {
    std::vector<int> log;
    std::vector<std::unique_ptr<sc::MonotonicResource>> arenas;
    for (int i = 0; i < 1100; ++i) {
      arenas.push_back(pool.acquire());
      arenas.back()->make<Tracked>(&log, i);
    }

    for (auto &arena: arenas) {
      pool.release(std::move(arena));
    }
    CHECK(log.size() == 1100);
  }

  SECTION("releasing nothing is fine") { pool.release(nullptr); }
}
