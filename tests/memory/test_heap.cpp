#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory_resource>
#include <new>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <mimalloc.h>

#include "sc/memory/heap.hpp"

namespace {

  bool isAligned(const void *ptr, size_t alignment) { return reinterpret_cast<uintptr_t>(ptr) % alignment == 0; }

  struct Trivial {
    int a;
    double b;
  };

  struct alignas(64) Wide {
    std::byte data[64];
  };

  // Writes its id into a log when it is destroyed.
  struct Tracked {
    std::vector<int> *log;
    int id;

    Tracked(std::vector<int> *log, int id) : log(log), id(id) {}
    ~Tracked() { log->push_back(id); }
  };

  // A type with a destructor and a chosen alignment and size, which notes the
  // address it is destroyed at.
  template <size_t Align, size_t Size> struct alignas(Align) Probe {
    static inline std::vector<const void *> destroyed;

    unsigned char data[Size];

    ~Probe() { destroyed.push_back(this); }
  };

  // Counts what passes through it on the way to the default resource.
  struct CountingResource : std::pmr::memory_resource {
    size_t allocations = 0;
    size_t deallocations = 0;

  private:
    void *do_allocate(size_t bytes, size_t alignment) override {
      ++allocations;
      return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void *ptr, size_t bytes, size_t alignment) override {
      ++deallocations;
      std::pmr::new_delete_resource()->deallocate(ptr, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const memory_resource &other) const noexcept override { return this == &other; }
  };

  // Looks like a pmr container to the heap and notes when it is destroyed.
  struct FakeContainer {
    using value_type = int;
    using allocator_type = std::pmr::polymorphic_allocator<int>;

    static inline int destroyed = 0;

    allocator_type allocator;

    FakeContainer() = default;
    explicit FakeContainer(allocator_type allocator) : allocator(allocator) {}
    ~FakeContainer() { ++destroyed; }

    [[nodiscard]] allocator_type get_allocator() const { return allocator; }
  };

  // Creates a few objects, overwrites them completely and checks that each one
  // is aligned and destroyed again at the address make() returned.
  template <size_t Align, size_t Size> void checkProbe() {
    using Type = Probe<Align, Size>;
    static_assert(alignof(Type) == Align);
    Type::destroyed.clear();

    std::vector<const void *> made;
    {
      sc::Heap heap;
      for (int i = 0; i < 20; ++i) {
        Type *object = heap.make<Type>();
        REQUIRE(isAligned(object, Align));
        REQUIRE(heap.contains(object));
        // Must not reach what the heap keeps behind the object.
        std::memset(object->data, 0xEE, Size);
        made.push_back(object);
      }
      REQUIRE(Type::destroyed.empty());
    }

    std::ranges::reverse(made);
    CHECK(Type::destroyed == made);
  }

} // namespace

static_assert(!std::is_copy_constructible_v<sc::Heap>);
static_assert(!std::is_copy_assignable_v<sc::Heap>);
// Containers keep its address as their resource.
static_assert(!std::is_move_constructible_v<sc::Heap>);
static_assert(!std::is_move_assignable_v<sc::Heap>);

// makePmr is only there for types that take the allocator.
template <typename T>
concept PmrMakeable = requires(sc::Heap heap) { heap.template makePmr<T>(); };
static_assert(PmrMakeable<std::pmr::vector<int>>);
static_assert(PmrMakeable<std::pmr::string>);
static_assert(!PmrMakeable<std::vector<int>>);
static_assert(!PmrMakeable<std::string>);
static_assert(!PmrMakeable<int>);

// Which containers the heap may drop without their destructor.
static_assert(sc::detail::PmrContainer<std::pmr::vector<int>>);
static_assert(sc::detail::PmrContainer<std::pmr::string>);
static_assert(sc::detail::PmrContainer<std::pmr::map<int, int>>);
static_assert(!sc::detail::PmrContainer<std::vector<int>>);
static_assert(!sc::detail::PmrContainer<std::string>);
static_assert(!sc::detail::PmrContainer<int>);
static_assert(sc::detail::ownsOnlyResourceMemory<std::pmr::vector<int>>);
static_assert(sc::detail::ownsOnlyResourceMemory<std::pmr::string>);
static_assert(sc::detail::ownsOnlyResourceMemory<std::pmr::vector<std::pmr::string>>);
static_assert(sc::detail::ownsOnlyResourceMemory<std::pmr::map<int, std::pmr::string>>);
static_assert(sc::detail::ownsOnlyResourceMemory<std::pmr::unordered_map<std::pmr::string, std::pmr::vector<int>>>);
static_assert(!sc::detail::ownsOnlyResourceMemory<std::pmr::vector<Tracked>>);
static_assert(!sc::detail::ownsOnlyResourceMemory<std::pmr::vector<std::string>>);
static_assert(!sc::detail::ownsOnlyResourceMemory<std::pmr::map<int, std::vector<int>>>);
static_assert(!sc::detail::ownsOnlyResourceMemory<std::pmr::vector<std::pmr::vector<Tracked>>>);

TEST_CASE("Heap: allocations are aligned and do not overlap", "[memory][heap]") {
  sc::Heap heap;

  std::vector<std::pair<std::byte *, size_t>> chunks;
  for (size_t alignment : {size_t{1}, size_t{8}, size_t{16}, size_t{64}, size_t{4096}}) {
    for (size_t bytes : {size_t{1}, size_t{24}, size_t{100}, size_t{5000}}) {
      auto *ptr = static_cast<std::byte *>(heap.allocate(bytes, alignment));
      REQUIRE(ptr != nullptr);
      CHECK(isAligned(ptr, alignment));
      CHECK(heap.contains(ptr));
      std::memset(ptr, 0xAB, bytes);
      chunks.emplace_back(ptr, bytes);
    }
  }

  std::ranges::sort(chunks);
  for (size_t i = 1; i < chunks.size(); ++i) {
    CHECK(chunks[i - 1].first + chunks[i - 1].second <= chunks[i].first);
  }
}

TEST_CASE("Heap: the default alignment fits every basic type", "[memory][heap]") {
  sc::Heap heap;

  for (size_t bytes : {size_t{16}, size_t{64}, size_t{1000}}) {
    CHECK(isAligned(heap.allocate(bytes), alignof(std::max_align_t)));
  }
}

TEST_CASE("Heap: an alignment known at compile time", "[memory][heap]") {
  sc::Heap heap;

  SECTION("up to the one of the basic types") {
    for (size_t bytes : {size_t{1}, size_t{24}, size_t{5000}}) {
      CHECK(isAligned(heap.allocate<1>(bytes), 1));
      CHECK(isAligned(heap.allocate<8>(bytes), 8));
      void *ptr = heap.allocate<alignof(std::max_align_t)>(bytes);
      CHECK(isAligned(ptr, alignof(std::max_align_t)));
      CHECK(heap.contains(ptr));
    }
  }

  SECTION("stricter than that") {
    for (size_t bytes : {size_t{1}, size_t{64}, size_t{192}, size_t{5000}}) {
      CHECK(isAligned(heap.allocate<32>(bytes), 32));
      CHECK(isAligned(heap.allocate<64>(bytes), 64));
      void *ptr = heap.allocate<4096>(bytes);
      CHECK(isAligned(ptr, 4096));
      CHECK(heap.contains(ptr));
      std::memset(ptr, 0xCD, bytes);
    }
  }

  SECTION("too large a request fails") {
    constexpr size_t Huge = std::numeric_limits<size_t>::max() - 64;
    CHECK_THROWS_AS(heap.allocate<8>(Huge), std::bad_alloc);
    CHECK_THROWS_AS(heap.allocate<64>(Huge), std::bad_alloc);
  }
}

TEST_CASE("Heap: a request too large to exist fails", "[memory][heap]") {
  sc::Heap heap;
  constexpr size_t Huge = std::numeric_limits<size_t>::max() - 64;

  CHECK_THROWS_AS(heap.allocate(Huge), std::bad_alloc);
  CHECK_THROWS_AS(heap.allocate(Huge, 64), std::bad_alloc);
  CHECK_THROWS_AS(heap.allocateSpan<uint64_t>(Huge / 4), std::bad_alloc);

  // The heap is still usable.
  CHECK(heap.contains(heap.allocate(32)));
}

TEST_CASE("Heap: make constructs the object in the heap", "[memory][heap]") {
  sc::Heap heap;

  auto *trivial = heap.make<Trivial>(7, 2.5);
  REQUIRE(trivial != nullptr);
  CHECK(trivial->a == 7);
  CHECK(trivial->b == 2.5);
  CHECK(heap.contains(trivial));

  auto *wide = heap.make<Wide>();
  CHECK(isAligned(wide, 64));
  CHECK(heap.contains(wide));

  auto *text = heap.make<std::string>(40, 'x');
  CHECK(*text == std::string(40, 'x'));
  CHECK(heap.contains(text));
}

TEST_CASE("Heap: reset destroys the objects in reverse order", "[memory][heap]") {
  std::vector<int> log;
  sc::Heap heap;

  heap.make<Tracked>(&log, 1);
  heap.make<Trivial>(0, 0.0);
  heap.make<Tracked>(&log, 2);
  (void)heap.allocate(100);
  heap.make<Tracked>(&log, 3);
  REQUIRE(log.empty());

  heap.reset();
  CHECK(log == std::vector<int>{3, 2, 1});

  // Nothing is destroyed a second time.
  heap.reset();
  CHECK(log.size() == 3);

  SECTION("and the heap takes new objects") {
    heap.make<Tracked>(&log, 4);
    heap.reset();
    CHECK(log == std::vector<int>{3, 2, 1, 4});
  }
}

TEST_CASE("Heap: the destructor destroys the objects", "[memory][heap]") {
  std::vector<int> log;
  {
    sc::Heap heap;
    heap.make<Tracked>(&log, 1);
    heap.make<Tracked>(&log, 2);
    REQUIRE(log.empty());
  }

  CHECK(log == std::vector<int>{2, 1});
}

TEST_CASE("Heap: an object whose constructor throws is not destroyed", "[memory][heap]") {
  struct Throwing {
    std::vector<int> *log;

    explicit Throwing(std::vector<int> *log) : log(log) { throw 42; }
    ~Throwing() { log->push_back(-1); }
  };

  std::vector<int> log;
  sc::Heap heap;

  heap.make<Tracked>(&log, 1);
  CHECK_THROWS_AS(heap.make<Throwing>(&log), int);
  heap.make<Tracked>(&log, 2);

  heap.reset();
  CHECK(log == std::vector<int>{2, 1});
}

TEST_CASE("Heap: objects with a destructor keep their alignment", "[memory][heap]") {
  // Sizes that are and are not a multiple of what the heap puts behind them.
  checkProbe<1, 1>();
  checkProbe<1, 3>();
  checkProbe<2, 6>();
  checkProbe<4, 12>();
  checkProbe<8, 8>();
  checkProbe<8, 24>();
  checkProbe<16, 16>();
  checkProbe<16, 48>();
  checkProbe<32, 96>();
  checkProbe<64, 64>();
  checkProbe<64, 192>();
  checkProbe<4096, 4096>();
}

TEST_CASE("Heap: an object created by a destructor is destroyed as well", "[memory][heap]") {
  struct Spawning {
    sc::Heap *heap;
    std::vector<int> *log;

    ~Spawning() {
      log->push_back(1);
      heap->make<Tracked>(log, 2);
    }
  };

  std::vector<int> log;
  sc::Heap heap;
  heap.make<Spawning>(&heap, &log);

  heap.reset();

  CHECK(log == std::vector<int>{1, 2});
}

TEST_CASE("Heap: make destroys every pmr container it created", "[memory][heap]") {
  FakeContainer::destroyed = 0;

  SECTION("whatever resource it uses, this heap included") {
    CountingResource other;
    {
      sc::Heap heap;
      sc::Heap second;
      heap.make<FakeContainer>(&heap);
      heap.make<FakeContainer>(&other);
      heap.make<FakeContainer>(&second);
      heap.make<FakeContainer>();
      heap.reset();
      CHECK(FakeContainer::destroyed == 4);
    }
    CHECK(FakeContainer::destroyed == 4);
  }

  SECTION("the heap as the value of the elements") {
    CountingResource counting;
    std::pmr::memory_resource *before = std::pmr::set_default_resource(&counting);
    {
      sc::Heap heap;
      auto *heaps = heap.make<std::pmr::vector<sc::Heap *>>(3, &heap);
      REQUIRE(heaps->size() == 3);
      CHECK((*heaps)[2] == &heap);
      CHECK(heaps->get_allocator().resource() == &counting);
      // Not exactly one: the debug MSVC standard library allocates an iterator
      // proxy next to the elements.
      CHECK(counting.allocations >= 1);
      CHECK(counting.deallocations == 0);
    }
    std::pmr::set_default_resource(before);
    CHECK(counting.deallocations == counting.allocations);
  }

  SECTION("containers in the heap work all the same") {
    sc::Heap heap;
    auto *values = heap.make<std::pmr::vector<int>>(&heap);
    auto *texts = heap.make<std::pmr::vector<std::pmr::string>>(&heap);
    for (int i = 0; i < 1000; ++i) {
      values->push_back(i);
      texts->emplace_back("text number " + std::to_string(i) + ", long enough to leave the small buffer");
    }

    CHECK(values->back() == 999);
    CHECK(heap.contains(values->data()));
    CHECK(heap.contains(texts->back().data()));

    heap.reset();
    CHECK(heap.contains(heap.allocate(16)));
  }
}

TEST_CASE("Heap: makePmr puts the heap in as the allocator", "[memory][heap]") {
  sc::Heap heap;

  SECTION("without further arguments") {
    auto *values = heap.makePmr<std::pmr::vector<int>>();

    CHECK(values->get_allocator().resource() == &heap);
    values->resize(1000, 7);
    CHECK(heap.contains(values));
    CHECK(heap.contains(values->data()));
    CHECK(values->back() == 7);
  }

  SECTION("behind the arguments of the caller") {
    auto *text = heap.makePmr<std::pmr::string>("a string that is too long for the small buffer");
    auto *values = heap.makePmr<std::pmr::vector<int>>(100, 3);
    auto *listed = heap.makePmr<std::pmr::vector<int>>(std::initializer_list<int>{1, 2});

    CHECK(*text == "a string that is too long for the small buffer");
    CHECK(heap.contains(text->data()));
    CHECK(values->size() == 100);
    CHECK(heap.contains(values->data()));
    CHECK(listed->size() == 2);
    CHECK(listed->get_allocator().resource() == &heap);
  }

  SECTION("and hands it on to the elements") {
    auto *texts = heap.makePmr<std::pmr::vector<std::pmr::string>>();
    auto *named = heap.makePmr<std::pmr::map<int, std::pmr::string>>();
    for (int i = 0; i < 100; ++i) {
      texts->emplace_back("text number " + std::to_string(i) + ", long enough to leave the small buffer");
      named->emplace(i, "a string that is too long for the small buffer");
    }

    CHECK(texts->back().get_allocator().resource() == &heap);
    CHECK(heap.contains(texts->back().data()));
    CHECK(named->at(42).get_allocator().resource() == &heap);
    CHECK(heap.contains(named->at(42).data()));
  }

  SECTION("a container copied or moved in ends up in the heap") {
    CountingResource other;
    std::pmr::vector<int> source({1, 2, 3}, &other);

    auto *copy = heap.makePmr<std::pmr::vector<int>>(source);
    auto *moved = heap.makePmr<std::pmr::vector<int>>(std::move(source));

    CHECK(copy->get_allocator().resource() == &heap);
    CHECK(heap.contains(copy->data()));
    CHECK(moved->get_allocator().resource() == &heap);
    CHECK(heap.contains(moved->data()));
    CHECK(moved->size() == 3);
    CHECK(moved->back() == 3);
  }

  SECTION("a container that only holds heap memory gets no destructor call") {
    FakeContainer::destroyed = 0;
    heap.makePmr<FakeContainer>();
    heap.reset();
    CHECK(FakeContainer::destroyed == 0);
  }

  SECTION("and its block has no room for a node") {
    auto *viaPmr = heap.makePmr<std::pmr::vector<int>>();
    auto *viaMake = heap.make<std::pmr::vector<int>>(&heap);

    CHECK(mi_usable_size(viaPmr) < mi_usable_size(viaMake));
    CHECK(mi_usable_size(viaPmr) < sizeof(std::pmr::vector<int>) + 16);
  }

  SECTION("elements with a destructor are still destroyed") {
    std::vector<int> log;
    auto *objects = heap.makePmr<std::pmr::vector<Tracked>>();
    objects->reserve(2);
    objects->emplace_back(&log, 1);
    objects->emplace_back(&log, 2);

    heap.reset();
    CHECK(log.size() == 2);
  }

  SECTION("a constructor that throws leaves nothing to destroy") {
    FakeContainer::destroyed = 0;
    CHECK_THROWS_AS(heap.makePmr<std::pmr::vector<int>>(std::numeric_limits<size_t>::max() / 8), std::exception);
    CHECK(heap.contains(heap.allocate(16)));
  }
}

TEST_CASE("Heap: a pmr container that needs its destructor gets it", "[memory][heap]") {
  SECTION("because it uses another resource") {
    CountingResource other;
    {
      sc::Heap heap;
      auto *values = heap.make<std::pmr::vector<int>>(&other);
      auto *texts = heap.make<std::pmr::vector<std::pmr::string>>(&other);
      values->resize(1000);
      texts->emplace_back("a string that is too long for the small buffer");
      REQUIRE(other.allocations >= 3);
      REQUIRE(other.deallocations < other.allocations);
    }
    CHECK(other.deallocations == other.allocations);
  }

  SECTION("because its elements have one") {
    std::vector<int> log;
    {
      sc::Heap heap;
      auto *objects = heap.make<std::pmr::vector<Tracked>>(&heap);
      objects->reserve(3);
      objects->emplace_back(&log, 1);
      objects->emplace_back(&log, 2);
      objects->emplace_back(&log, 3);
      REQUIRE(log.empty());
    }
    CHECK(log.size() == 3);
  }

  SECTION("because the elements of a nested container have one") {
    std::vector<int> log;
    {
      sc::Heap heap;
      auto *nested = heap.make<std::pmr::vector<std::pmr::vector<Tracked>>>(&heap);
      nested->emplace_back();
      nested->back().reserve(2);
      nested->back().emplace_back(&log, 1);
      nested->back().emplace_back(&log, 2);
      REQUIRE(log.empty());
    }
    CHECK(log.size() == 2);
  }
}

TEST_CASE("Heap: make is safe from several threads", "[memory][heap]") {
  constexpr int Threads = 8;
  constexpr int PerThread = 500;

  std::atomic<int> destroyed{0};
  struct Counted {
    std::atomic<int> *destroyed;
    std::string text;

    ~Counted() { destroyed->fetch_add(1, std::memory_order_relaxed); }
  };

  std::vector<std::vector<Counted *>> results(Threads);
  {
    sc::Heap heap;
    {
      std::vector<std::jthread> workers;
      for (int t = 0; t < Threads; ++t) {
        workers.emplace_back([&, t] {
          for (int i = 0; i < PerThread; ++i) {
            results[t].push_back(heap.make<Counted>(&destroyed, std::to_string((t * PerThread) + i)));
          }
        });
      }
    }

    size_t wrong = 0;
    for (int t = 0; t < Threads; ++t) {
      for (int i = 0; i < PerThread; ++i) {
        wrong += static_cast<size_t>(results[t][i]->text != std::to_string((t * PerThread) + i));
      }
    }
    CHECK(wrong == 0);
    CHECK(destroyed.load() == 0);
  }

  CHECK(destroyed.load() == Threads * PerThread);
}

TEST_CASE("Heap: allocateSpan", "[memory][heap]") {
  sc::Heap heap;

  SECTION("an empty span for a count of zero") {
    auto span = heap.allocateSpan<int>(0);
    CHECK(span.empty());
    CHECK(span.data() == nullptr);
  }

  SECTION("holds count elements with the alignment of the type") {
    auto span = heap.allocateSpan<double>(10);
    REQUIRE(span.size() == 10);
    CHECK(isAligned(span.data(), alignof(double)));
    for (size_t i = 0; i < span.size(); ++i) {
      span[i] = static_cast<double>(i);
    }
    CHECK(span[9] == 9.0);
  }

  SECTION("bytes by default") {
    auto span = heap.allocateSpan(5);
    CHECK(span.size_bytes() == 5);
  }

  SECTION("takes a stricter alignment") {
    auto span = heap.allocateSpan<uint8_t>(10, 64);
    CHECK(isAligned(span.data(), 64));
    CHECK(heap.allocateSpan<int>(0, 64).empty());
  }

  SECTION("of an over-aligned type") {
    auto span = heap.allocateSpan<Wide>(3);
    REQUIRE(span.size() == 3);
    CHECK(isAligned(span.data(), 64));
  }
}

TEST_CASE("Heap: is a pmr memory resource", "[memory][heap]") {
  sc::Heap heap;
  std::pmr::memory_resource *resource = &heap;

  SECTION("allocates from the heap") {
    void *plain = resource->allocate(100);
    void *aligned = resource->allocate(100, 256);

    CHECK(heap.contains(plain));
    CHECK(heap.contains(aligned));
    CHECK(isAligned(aligned, 256));

    resource->deallocate(plain, 100);
    resource->deallocate(aligned, 100, 256);
  }

  SECTION("fails like any other resource") {
    constexpr size_t Huge = std::numeric_limits<size_t>::max() - 64;
    CHECK_THROWS_AS(resource->allocate(Huge), std::bad_alloc);
  }

  SECTION("is only equal to itself") {
    sc::Heap other;

    CHECK(heap.is_equal(heap));
    CHECK_FALSE(heap.is_equal(other));
    CHECK_FALSE(heap.is_equal(*std::pmr::get_default_resource()));
  }

  SECTION("holds pmr containers") {
    std::pmr::vector<int> values(&heap);
    for (int i = 0; i < 100'000; ++i) {
      values.push_back(i);
    }
    std::pmr::string text("a string that is too long for the small buffer", &heap);
    std::pmr::vector<std::pmr::string> texts(&heap);
    for (int i = 0; i < 100; ++i) {
      texts.emplace_back("text number " + std::to_string(i) + ", long enough to leave the small buffer");
    }

    CHECK(values.back() == 99'999);
    CHECK(heap.contains(values.data()));
    CHECK(heap.contains(text.data()));
    CHECK(heap.contains(texts.data()));
    CHECK(heap.contains(texts[99].data()));
    CHECK(texts[42] == "text number 42, long enough to leave the small buffer");
  }

  SECTION("gives back what a container deallocates") {
    // The buffers a growing vector leaves behind are reused, so a small
    // vector that is filled and dropped again and again needs no new pages.
    void *first = nullptr;
    size_t differentPages = 0;
    for (int round = 0; round < 1000; ++round) {
      std::pmr::vector<uint64_t> values(&heap);
      values.resize(64);
      if (first == nullptr) {
        first = values.data();
      }
      const auto distance = reinterpret_cast<uintptr_t>(values.data()) > reinterpret_cast<uintptr_t>(first)
                                ? reinterpret_cast<uintptr_t>(values.data()) - reinterpret_cast<uintptr_t>(first)
                                : reinterpret_cast<uintptr_t>(first) - reinterpret_cast<uintptr_t>(values.data());
      differentPages += static_cast<size_t>(distance >= static_cast<uintptr_t>(64 * 1024));
    }
    CHECK(differentPages == 0);
  }
}

TEST_CASE("Heap: contains only its own blocks", "[memory][heap]") {
  sc::Heap a;
  sc::Heap b;
  int onStack = 0;

  void *fromA = a.allocate(64);
  void *fromB = b.allocate(64);

  CHECK(a.contains(fromA));
  CHECK_FALSE(a.contains(fromB));
  CHECK(b.contains(fromB));
  CHECK_FALSE(b.contains(fromA));
  CHECK_FALSE(a.contains(&onStack));
}

TEST_CASE("Heap: reset frees everything and leaves a usable heap", "[memory][heap]") {
  sc::Heap heap;

  std::vector<void *> blocks;
  for (int i = 0; i < 1000; ++i) {
    blocks.push_back(heap.allocate(256));
  }
  heap.reset();

  REQUIRE(heap.get() != nullptr);
  size_t stillOwned = 0;
  for (void *block : blocks) {
    stillOwned += static_cast<size_t>(heap.contains(block));
  }
  CHECK(stillOwned == 0);

  void *after = heap.allocate(256);
  CHECK(heap.contains(after));

  SECTION("again and again") {
    for (int round = 0; round < 200; ++round) {
      auto span = heap.allocateSpan<uint8_t>(10'000);
      std::memset(span.data(), round, span.size());
      heap.reset();
    }
    CHECK(heap.contains(heap.allocate(16)));
  }
}

TEST_CASE("Heap: concurrent allocations do not overlap", "[memory][heap]") {
  constexpr size_t Threads = 8;
  constexpr size_t PerThread = 2000;
  constexpr size_t Bytes = 24;

  sc::Heap heap;
  std::vector<std::vector<std::byte *>> results(Threads);
  {
    std::vector<std::jthread> workers;
    for (size_t t = 0; t < Threads; ++t) {
      workers.emplace_back([&, t] {
        results[t].reserve(PerThread);
        for (size_t i = 0; i < PerThread; ++i) {
          auto *ptr = static_cast<std::byte *>(heap.allocate(Bytes));
          std::memset(ptr, static_cast<int>(t + 1), Bytes);
          results[t].push_back(ptr);
        }
      });
    }
  }

  // Every chunk still holds the pattern of the thread that got it.
  std::vector<std::byte *> all;
  size_t corrupted = 0;
  size_t foreign = 0;
  for (size_t t = 0; t < Threads; ++t) {
    for (std::byte *ptr : results[t]) {
      for (size_t i = 0; i < Bytes; ++i) {
        corrupted += static_cast<size_t>(ptr[i] != static_cast<std::byte>(t + 1));
      }
      foreign += static_cast<size_t>(!heap.contains(ptr));
      all.push_back(ptr);
    }
  }
  CHECK(corrupted == 0);
  CHECK(foreign == 0);

  std::ranges::sort(all);
  size_t overlaps = 0;
  for (size_t i = 1; i < all.size(); ++i) {
    overlaps += static_cast<size_t>(all[i - 1] + Bytes > all[i]);
  }
  CHECK(overlaps == 0);

  SECTION("and another thread may reset the heap afterwards") {
    std::thread([&] { heap.reset(); }).join();
    CHECK(heap.contains(heap.allocate(64)));
  }
}
