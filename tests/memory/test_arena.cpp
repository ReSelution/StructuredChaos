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

#include "sc/memory/arena.hpp"
#include "sc/memory/arena_pool.hpp"

namespace {

bool isAligned(const void *ptr, size_t alignment) {
  return reinterpret_cast<uintptr_t>(ptr) % alignment == 0;
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

TEST_CASE("Arena: make constructs the object in the arena", "[memory][arena]") {
  sc::Arena arena(1024);

  auto *trivial = arena.make<Trivial>(7, 2.5);
  REQUIRE(trivial != nullptr);
  CHECK(trivial->a == 7);
  CHECK(trivial->b == 2.5);
  CHECK(isAligned(trivial, alignof(Trivial)));

  auto *text = arena.make<std::string>(40, 'x');
  CHECK(*text == std::string(40, 'x'));

  auto *wide = arena.make<Wide>();
  CHECK(isAligned(wide, 64));
}

TEST_CASE("Arena: reset destroys the objects in reverse order",
          "[memory][arena]") {
  std::vector<int> log;
  sc::Arena arena(1024);

  arena.make<Tracked>(&log, 1);
  arena.make<Trivial>(0, 0.0);
  arena.make<Tracked>(&log, 2);
  arena.make<Tracked>(&log, 3);
  REQUIRE(log.empty());

  arena.reset();
  CHECK(log == std::vector<int>{3, 2, 1});

  // Nothing is destroyed a second time.
  arena.reset();
  CHECK(log.size() == 3);
}

TEST_CASE("Arena: the destructor destroys the objects", "[memory][arena]") {
  std::vector<int> log;
  {
    sc::Arena arena(1024);
    arena.make<Tracked>(&log, 1);
    arena.make<Tracked>(&log, 2);
    REQUIRE(log.empty());
  }

  CHECK(log == std::vector<int>{2, 1});
}

TEST_CASE("Arena: an object whose constructor throws is not destroyed",
          "[memory][arena]") {
  struct Throwing {
    std::vector<int> *log;

    explicit Throwing(std::vector<int> *log) : log(log) { throw 42; }
    ~Throwing() { log->push_back(-1); }
  };

  std::vector<int> log;
  sc::Arena arena(1024);

  arena.make<Tracked>(&log, 1);
  CHECK_THROWS_AS(arena.make<Throwing>(&log), int);
  arena.make<Tracked>(&log, 2);

  arena.reset();
  CHECK(log == std::vector<int>{2, 1});
}

TEST_CASE("Arena: reset makes the memory available again", "[memory][arena]") {
  sc::Arena arena(1024);

  void *first = arena.allocate(64, 8);
  (void)arena.allocate(4096, 8);
  arena.reset();

  CHECK(arena.allocate(64, 8) == first);
}

TEST_CASE("Arena: grows beyond its initial size", "[memory][arena]") {
  std::vector<int> log;
  sc::Arena arena(128);

  std::vector<std::span<uint8_t>> spans;
  for (int i = 0; i < 100; ++i) {
    auto span = arena.allocateSpan<uint8_t>(100);
    std::memset(span.data(), i, span.size());
    spans.push_back(span);
    arena.make<Tracked>(&log, i);
  }

  size_t corrupted = 0;
  for (int i = 0; i < 100; ++i) {
    for (uint8_t byte : spans[i]) {
      corrupted += byte != static_cast<uint8_t>(i);
    }
  }
  CHECK(corrupted == 0);

  arena.reset();
  CHECK(log.size() == 100);
}

TEST_CASE("Arena: allocateSpan", "[memory][arena]") {
  sc::Arena arena(1024);

  SECTION("an empty span for a count of zero") {
    auto span = arena.allocateSpan<int>(0);
    CHECK(span.empty());
    CHECK(span.data() == nullptr);
  }

  SECTION("holds count elements with the alignment of the type") {
    auto span = arena.allocateSpan<double>(10);
    REQUIRE(span.size() == 10);
    CHECK(isAligned(span.data(), alignof(double)));
    for (size_t i = 0; i < span.size(); ++i) {
      span[i] = static_cast<double>(i);
    }
    CHECK(span[9] == 9.0);
  }

  SECTION("bytes by default") {
    auto span = arena.allocateSpan(5);
    CHECK(span.size_bytes() == 5);
  }

  SECTION("takes a stricter alignment") {
    auto span = arena.allocateSpan<uint8_t>(10, 64);
    CHECK(isAligned(span.data(), 64));
  }
}

TEST_CASE("Arena: resource feeds pmr containers", "[memory][arena]") {
  sc::Arena arena(1024);

  std::pmr::vector<int> values(arena.resource());
  for (int i = 0; i < 500; ++i) {
    values.push_back(i);
  }
  std::pmr::string text("a string that is too long for the small buffer",
                        arena.resource());

  CHECK(values.back() == 499);
  CHECK(text.size() == 46);
}

TEST_CASE("Arena: utf16ToUtf8", "[memory][arena]") {
  sc::Arena arena(1024);

  SECTION("ASCII") {
    std::u16string_view input = u"Hello";
    auto result = arena.utf16ToUtf8(input);
    CHECK(result == "Hello");
    CHECK(result.data()[result.size()] == '\0');
  }

  SECTION("two and three byte characters") {
    auto result = arena.utf16ToUtf8(std::u16string_view(u"Grüße €"));
    CHECK(result == "Gr\xC3\xBC\xC3\x9F"
                    "e \xE2\x82\xAC");
    CHECK(result.data()[result.size()] == '\0');
  }

  SECTION("surrogate pairs") {
    auto result = arena.utf16ToUtf8(std::u16string_view(u"\U0001F600"));
    CHECK(result == "\xF0\x9F\x98\x80");
  }

  SECTION("empty input") {
    CHECK(arena.utf16ToUtf8({}).empty());
  }

  SECTION("stays valid across further allocations") {
    auto first = arena.utf16ToUtf8(std::u16string_view(u"first"));
    auto second = arena.utf16ToUtf8(std::u16string_view(u"second"));
    (void)arena.allocate(8192, 8);
    CHECK(first == "first");
    CHECK(second == "second");
  }

  SECTION("invalid input gives an empty string") {
    const char16_t loneHigh[] = {u'a', 0xD800, u'b'};
    const char16_t loneLow[] = {0xDC00};
    const char16_t swapped[] = {0xDC00, 0xD800};

    for (std::span<const char16_t> input :
         {std::span<const char16_t>(loneHigh),
          std::span<const char16_t>(loneLow),
          std::span<const char16_t>(swapped)}) {
      auto result = arena.utf16ToUtf8(input);
      CHECK(result.empty());
      REQUIRE(result.data() != nullptr);
      CHECK(*result.data() == '\0');
    }
  }
}

TEST_CASE("Arena: make is safe from several threads", "[memory][arena]") {
  constexpr int Threads = 8;
  constexpr int PerThread = 500;

  sc::Arena arena(4096);
  std::vector<std::vector<std::string *>> results(Threads);
  {
    std::vector<std::jthread> workers;
    for (int t = 0; t < Threads; ++t) {
      workers.emplace_back([&, t] {
        for (int i = 0; i < PerThread; ++i) {
          results[t].push_back(
              arena.make<std::string>(std::to_string(t * PerThread + i)));
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

TEST_CASE("ArenaPool: hands out and takes back arenas", "[memory][arena]") {
  sc::ArenaPool::init(2, 2048);
  CHECK(sc::ArenaPool::getBufferSize() == 2048);

  SECTION("acquire always returns an arena") {
    std::vector<std::unique_ptr<sc::Arena>> arenas;
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
    sc::Arena *address = arena.get();
    void *first = arena->allocate(64, 8);
    sc::ArenaPool::release(arena);

    auto again = sc::ArenaPool::acquire();
    CHECK(again.get() == address);
    CHECK(again->allocate(64, 8) == first);
    sc::ArenaPool::release(again);
  }

  SECTION("more arenas than the pool keeps can be released") {
    std::vector<int> log;
    std::vector<std::unique_ptr<sc::Arena>> arenas;
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
    std::unique_ptr<sc::Arena> none;
    sc::ArenaPool::release(none);
    CHECK(none == nullptr);
  }
}
