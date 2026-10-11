#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <thread>
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
  sc::ArenaPool::init(2, 2048);
  CHECK(sc::ArenaPool::getBufferSize() == 2048);

  SECTION("acquire always returns an arena") {
    std::vector<std::unique_ptr<sc::MonotonicResource>> arenas;
    // More than the pool was filled with.
    for (int i = 0; i < 8; ++i) {
      arenas.push_back(sc::ArenaPool::acquire());
      REQUIRE(arenas.back() != nullptr);
      CHECK(arenas.back()->allocate(64, 8) != nullptr);
    }
    for (auto &arena : arenas) {
      sc::ArenaPool::release(arena);
    }
  }

  SECTION("release takes the arena and destroys its objects") {
    std::vector<int> log;
    auto arena = sc::ArenaPool::acquire();
    arena->make<Tracked>(&log, 1);

    sc::ArenaPool::release(arena);

    CHECK(arena == nullptr);
    CHECK(log == std::vector<int>{1});
  }

  SECTION("a released arena is handed out again, empty") {
    auto arena = sc::ArenaPool::acquire();
    sc::MonotonicResource *address = arena.get();
    void *first = arena->allocate(64, 8);
    sc::ArenaPool::release(arena);

    auto again = sc::ArenaPool::acquire();
    CHECK(again.get() == address);
    CHECK(again->allocate(64, 8) == first);
    sc::ArenaPool::release(again);
  }

  SECTION("more arenas than the pool keeps can be released") {
    std::vector<int> log;
    std::vector<std::unique_ptr<sc::MonotonicResource>> arenas;
    for (int i = 0; i < 1100; ++i) {
      arenas.push_back(sc::ArenaPool::acquire());
      arenas.back()->make<Tracked>(&log, i);
    }

    for (auto &arena : arenas) {
      sc::ArenaPool::release(arena);
      REQUIRE(arena == nullptr);
    }
    CHECK(log.size() == 1100);

    // Leaves the pool as the other sections expect it.
    for (int i = 0; i < 1100; ++i) {
      arenas[i] = sc::ArenaPool::acquire();
    }
  }

  SECTION("releasing nothing is fine") {
    std::unique_ptr<sc::MonotonicResource> none;
    sc::ArenaPool::release(none);
    CHECK(none == nullptr);
  }
}
