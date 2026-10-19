#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "sc/memory/deferred.hpp"

namespace {

  using sc::memory::DeletionQueue;

} // namespace

TEST_CASE("Deletion Queue", "[memory][deferred]") {
  SECTION("called once the slot of its frame comes again") {
    DeletionQueue queue(2);
    int called = 0;
    queue.beginFrame(0);
    queue.push([&] { ++called; });

    queue.beginFrame(1);
    CHECK(called == 0);
    queue.beginFrame(2);
    CHECK(called == 1);
    queue.beginFrame(3);
    queue.beginFrame(4);
    CHECK(called == 1);
  }

  SECTION("in the order they were pushed") {
    DeletionQueue queue(1);
    std::vector<int> order;
    for (int i = 0; i < 5; ++i) {
      queue.push([&order, i] { order.push_back(i); });
    }
    queue.beginFrame(1);
    CHECK(order == std::vector<int>{0, 1, 2, 3, 4});
  }

  SECTION("captures are destroyed after the call") {
    DeletionQueue queue(1);
    auto owned = std::make_shared<int>(7);
    const std::weak_ptr<int> watch = owned;
    queue.push([owned = std::move(owned)] { CHECK(*owned == 7); });
    CHECK(!watch.expired());
    queue.flushAll();
    CHECK(watch.expired());
  }

  SECTION("what a function pushes is called in the same flush") {
    DeletionQueue queue(3);
    int called = 0;
    queue.push([&] { queue.push([&] { ++called; }); });
    queue.flushAll();
    CHECK(called == 1);
  }

  SECTION("from many threads, each once") {
    constexpr size_t THREADS = 8;
    constexpr size_t PUSHES = 10'000;
    std::atomic<size_t> called{0};
    {
      DeletionQueue queue(3);
      std::vector<std::thread> threads;
      for (size_t t = 0; t < THREADS; ++t) {
        threads.emplace_back([&] {
          for (size_t i = 0; i < PUSHES; ++i) {
            queue.push([&] { called.fetch_add(1, std::memory_order_relaxed); });
          }
        });
      }
      for (std::thread &thread: threads) {
        thread.join();
      }
      CHECK(called.load() == 0);
      // The destructor calls what is left.
    }
    CHECK(called.load() == THREADS * PUSHES);
  }
}
