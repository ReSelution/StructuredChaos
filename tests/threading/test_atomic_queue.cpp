#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include "sc/threading/atomic_queue.hpp"

namespace {

struct MoveOnlyType {
  int value;
  explicit MoveOnlyType(int v) : value(v) {}
  MoveOnlyType(const MoveOnlyType &) = delete;
  MoveOnlyType &operator=(const MoveOnlyType &) = delete;
  MoveOnlyType(MoveOnlyType &&) noexcept = default;
  MoveOnlyType &operator=(MoveOnlyType &&) noexcept = default;
};

struct LifetimeTracker {
  inline static std::atomic<int> instances{0};
  int value{0};

  explicit LifetimeTracker(int v) : value(v) {
    instances.fetch_add(1, std::memory_order_relaxed);
  }
  ~LifetimeTracker() { instances.fetch_sub(1, std::memory_order_relaxed); }

  LifetimeTracker(const LifetimeTracker &o) : value(o.value) {
    instances.fetch_add(1, std::memory_order_relaxed);
  }
  LifetimeTracker(LifetimeTracker &&o) noexcept : value(o.value) {
    instances.fetch_add(1, std::memory_order_relaxed);
  }
  LifetimeTracker &operator=(const LifetimeTracker &) = default;
  LifetimeTracker &operator=(LifetimeTracker &&) noexcept = default;
};

} // namespace

TEST_CASE("AtomicQueue Basic Operations", "[threading][atomic_queue]") {
  using namespace sc::threading;

  SECTION("Push and Pop Single Element") {
    AtomicQueue<int, 64> queue;
    REQUIRE(queue.empty());
    REQUIRE(queue.size() == 0);

    queue.push(42);
    REQUIRE_FALSE(queue.empty());
    REQUIRE(queue.size() == 1);

    int result = 0;
    bool success = queue.try_pop(result, 0);
    REQUIRE(success);
    REQUIRE(result == 42);
    REQUIRE(queue.empty());
  }

  SECTION("Move-Only Types") {
    AtomicQueue<MoveOnlyType, 64> queue;

    queue.emplace(100);
    queue.push(MoveOnlyType(200));

    MoveOnlyType out1(0);
    MoveOnlyType out2(0);

    REQUIRE(queue.try_pop(out1, 0));
    REQUIRE(out1.value == 100);

    REQUIRE(queue.try_pop(out2, 0));
    REQUIRE(out2.value == 200);
  }

  SECTION("FIFO Ordering") {
    AtomicQueue<int, 64> queue;
    constexpr int count = 10;

    for (int i = 0; i < count; ++i) {
      queue.push(std::move(i));
    }

    for (int i = 0; i < count; ++i) {
      int val = -1;
      REQUIRE(queue.try_pop(val, 0));
      REQUIRE(val == i);
    }

    int val = 0;
    REQUIRE_FALSE(queue.try_pop(val, 0));
  }
}

TEST_CASE("AtomicQueue Lifetime Management", "[threading][atomic_queue]") {
  using namespace sc::threading;

  SECTION("Destructor Cleans Up Remaining Items") {
    LifetimeTracker::instances.store(0);

    {
      AtomicQueue<LifetimeTracker, 64> queue;
      queue.emplace(1);
      queue.emplace(2);
      queue.emplace(3);

      REQUIRE(LifetimeTracker::instances.load() == 3);

      LifetimeTracker dummy(0);
      queue.try_pop(dummy, 0); // Zerstört ein Element vorzeitig
    }

    // Beim Verlassen des Scope muss der Queue-Destruktor die verbliebenen 2
    // Elemente aufräumen
    REQUIRE(LifetimeTracker::instances.load() == 0);
  }
}

TEST_CASE("AtomicQueue Stress and Wrap-Around", "[threading][atomic_queue]") {
  using namespace sc::threading;

  SECTION("Ring Buffer Wrap-Around Test") {
    constexpr size_t capacity = 16;
    AtomicQueue<int, capacity> queue;

    constexpr int total_iterations = 1000;
    for (int i = 0; i < total_iterations; ++i) {
      queue.push(std::move(i));
      int out = -1;
      REQUIRE(queue.try_pop(out, 0));
      REQUIRE(out == i);
    }
    REQUIRE(queue.empty());
  }

  SECTION("Multi-Threaded Producer/Consumer Stress Test") {
    constexpr size_t capacity = 1024;
    AtomicQueue<int, capacity> queue;

    constexpr int num_producers = 4;
    constexpr int num_consumers = 4;
    constexpr int items_per_producer = 50'000;
    constexpr int total_items = num_producers * items_per_producer;

    std::atomic<int> consumed_count{0};
    std::atomic<int64_t> sum_consumed{0};

    std::vector<std::thread> threads;

    // Producer Threads
    for (int p = 0; p < num_producers; ++p) {
      threads.emplace_back([&queue, p]() {
        for (int i = 0; i < items_per_producer; ++i) {
          queue.push(p * items_per_producer + i);
        }
      });
    }

    // Consumer Threads
    for (int c = 0; c < num_consumers; ++c) {
      threads.emplace_back([&queue, &consumed_count, &sum_consumed, c]() {
        int item = 0;
        while (consumed_count.load(std::memory_order_relaxed) < total_items) {
          if (queue.try_pop(item, c)) {
            sum_consumed.fetch_add(item, std::memory_order_relaxed);
            consumed_count.fetch_add(1, std::memory_order_relaxed);
          } else {
            std::this_thread::yield();
          }
        }
      });
    }

    for (auto &t : threads) {
      t.join();
    }

    REQUIRE(consumed_count.load() == total_items);

    // Summen-Abgleich zur Verifizierung der Datenintegrität
    int64_t expected_sum = 0;
    for (int i = 0; i < total_items; ++i) {
      expected_sum += i;
    }
    REQUIRE(sum_consumed.load() == expected_sum);
  }
}

TEST_CASE("AtomicQueue Memory Reclamation", "[threading][atomic_queue]") {
  using namespace sc::threading;

  struct LargeTask {
    char data[512]{1};
  };

  constexpr size_t capacity = 65536;
  using QueueType = AtomicQueue<LargeTask, capacity>;

  SECTION("Block Cycle Wrap-Around") {
    // Verifiziert, dass der Release-Counter-Mechanismus über mehrere
    // Zyklen hinweg exakt synchron mit den Pushes/Pops läuft
    QueueType queue;
    constexpr size_t items = QueueType::slots_per_block() * 2;

    for (int cycle = 0; cycle < 3; ++cycle) {
      for (size_t i = 0; i < items; ++i) {
        queue.push(LargeTask{});
      }

      LargeTask task;
      for (size_t i = 0; i < items; ++i) {
        REQUIRE(queue.try_pop(task, 0));
      }

      REQUIRE(queue.empty());
    }
  }

  SECTION("Multi-Threaded Stress Test Across Reclaimed Blocks") {
    // Small enough to wrap around many times, large enough to span several
    // blocks, so that blocks are reclaimed while producers and consumers run.
    using SmallQueue = AtomicQueue<int64_t, 4096>;
    STATIC_REQUIRE(SmallQueue::num_blocks() > 1);
    SmallQueue queue;

    constexpr int num_producers = 4;
    constexpr int num_consumers = 4;
    constexpr int items_per_producer = 100'000;
    constexpr int total_items = num_producers * items_per_producer;

    std::atomic<int> consumed_count{0};
    std::atomic<int64_t> sum_consumed{0};
    std::atomic<bool> order_ok{true};

    std::vector<std::thread> threads;

    for (int p = 0; p < num_producers; ++p) {
      threads.emplace_back([&queue, p]() {
        for (int i = 0; i < items_per_producer; ++i) {
          queue.push(static_cast<int64_t>(p) * items_per_producer + i);
        }
      });
    }

    for (int c = 0; c < num_consumers; ++c) {
      threads.emplace_back([&, c]() {
        // Items of one producer must reach a single consumer in push order.
        std::vector<int64_t> last_seen(num_producers, -1);
        int64_t item = 0;
        while (consumed_count.load(std::memory_order_relaxed) < total_items) {
          if (queue.try_pop(item, c)) {
            const auto producer = static_cast<size_t>(item / items_per_producer);
            if (item <= last_seen[producer]) {
              order_ok.store(false, std::memory_order_relaxed);
            }
            last_seen[producer] = item;
            sum_consumed.fetch_add(item, std::memory_order_relaxed);
            consumed_count.fetch_add(1, std::memory_order_relaxed);
          } else {
            std::this_thread::yield();
          }
        }
      });
    }

    for (auto &t : threads) {
      t.join();
    }

    const int64_t expected_sum =
        static_cast<int64_t>(total_items) * (total_items - 1) / 2;
    REQUIRE(consumed_count.load() == total_items);
    REQUIRE(sum_consumed.load() == expected_sum);
    REQUIRE(order_ok.load());
    REQUIRE(queue.empty());
  }
}
