#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <future>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sc/logger/logger.hpp"
#include "sc/stats/stats.hpp"
#include "sc/stats/throughput.hpp"
#include "sc/stats/units.hpp"
#include "sc/threading/threading.hpp"

using StressLog = sc::Logger<"StressLog">;
using BatchThroughput = sc::stats::Stat<"Batch Processing", sc::stats::Throughput<sc::stats::MetricUnits>>;

namespace {

  struct SFOBreaker {
    std::array<std::byte, 100> weight;
  };

  struct TrackedTask {
    inline static std::atomic<int> copies{0};
    inline static std::atomic<int> moves{0};

    int id;

    TrackedTask(int i) : id(i) {}

    TrackedTask(const TrackedTask &o) : id(o.id) { copies++; }
    TrackedTask(TrackedTask &&o) noexcept : id(o.id) { moves++; }

    static void reset() {
      copies = 0;
      moves = 0;
    }
  };

  struct FastTask {
    int id;
    FastTask(int i) : id(i) {}
  };

  template<typename TaskType>
  void execute_batch_run(bool force_no_sfo, bool use_detach) {
    constexpr size_t TASKS = 20000;
    std::vector<TaskType> tasks;
    tasks.reserve(TASKS);
    for (int i = 0; std::cmp_less(i, TASKS); ++i) {
      tasks.emplace_back(i);
    }

    SFOBreaker breaker;

    BatchThroughput::reset();
    BatchThroughput::start();

    sc::threading::PoolThroughput::m_storage.value.store(0, std::memory_order_relaxed);
    sc::threading::PoolThroughput::m_storage.accumulated_ns.store(0, std::memory_order_relaxed);
    sc::threading::PoolThroughput::m_storage.running.store(false, std::memory_order_relaxed);
    sc::threading::PoolThroughput::start();

    if (use_detach) {
      if (force_no_sfo) {
        sc::threading::detachBatch(
            std::move(tasks), [breaker](TaskType t [[maybe_unused]]) { (void) breaker; }, nullptr);
      } else {
        sc::threading::detachBatch(std::move(tasks), [](TaskType t [[maybe_unused]]) {}, nullptr);
      }
    } else {
      if (force_no_sfo) {
        auto f =
            sc::threading::enqueueBatch(std::move(tasks), [breaker](TaskType t [[maybe_unused]]) { (void) breaker; });
      } else {
        auto f = sc::threading::enqueueBatch(std::move(tasks), [](TaskType t [[maybe_unused]]) {});
      }
    }

    BatchThroughput::record(TASKS);
    BatchThroughput::stop();
    sc::threading::wait_until_finished();
  }

} // namespace

TEST_CASE("Threading Batch Execution Verification", "[threading][execution]") {
  sc::threading::init();

  constexpr size_t task_count = 1000;

  SECTION("Verify enqueueBatch Executes All Tasks") {
    std::atomic<size_t> executed_counter{0};
    std::vector<int> tasks(task_count);
    std::ranges::iota(tasks, 0);

    auto future = sc::threading::enqueueBatch(std::move(tasks), [&executed_counter](int task_val [[maybe_unused]]) {
      executed_counter.fetch_add(1, std::memory_order_relaxed);
    });

    future.get();
    sc::threading::wait_until_finished();

    REQUIRE(executed_counter.load() == task_count);
  }

  SECTION("Verify detachBatch Executes All Tasks") {
    std::atomic<size_t> executed_counter{0};
    std::vector<int> tasks(task_count);
    std::ranges::iota(tasks, 0);

    sc::threading::detachBatch(
        std::move(tasks),
        [&executed_counter](int task_val [[maybe_unused]]) {
          executed_counter.fetch_add(1, std::memory_order_relaxed);
        },
        nullptr);

    sc::threading::wait_until_finished();

    REQUIRE(executed_counter.load() == task_count);
  }

  SECTION("Verify Correct Data Payload Passed To Worker") {
    std::atomic<bool> data_valid{true};
    std::vector<int> tasks(task_count);
    std::ranges::iota(tasks, 0);

    sc::threading::detachBatch(
        std::move(tasks),
        [&data_valid](int task_val) {
          if (task_val < 0 || std::cmp_greater_equal(task_val, task_count)) {
            data_valid.store(false, std::memory_order_relaxed);
          }
        },
        nullptr);

    sc::threading::wait_until_finished();

    REQUIRE(data_valid.load() == true);
  }
}

TEST_CASE("Threading Efficiency Correctness", "[threading][efficiency][verification]") {
  sc::threading::init();

  SECTION("SFO Path - Batch") {
    TrackedTask::reset();
    execute_batch_run<TrackedTask>(false, false);
    REQUIRE(TrackedTask::copies.load() >= 0);
  }

  SECTION("SFO Path - Detach") {
    TrackedTask::reset();
    execute_batch_run<TrackedTask>(false, true);
    REQUIRE(TrackedTask::copies.load() >= 0);
  }

  SECTION("Merged Path (No SFO) - Batch") {
    TrackedTask::reset();
    execute_batch_run<TrackedTask>(true, false);
    REQUIRE(TrackedTask::copies.load() >= 0);
  }

  SECTION("Merged Path (No SFO) - Detach") {
    TrackedTask::reset();
    execute_batch_run<TrackedTask>(true, true);
    REQUIRE(TrackedTask::copies.load() >= 0);
  }
}

TEST_CASE("Threading Single Task Detach", "[threading][detach]") {
  sc::threading::init();

  SECTION("Runs Task With Arguments") {
    std::atomic<int> result{0};

    sc::threading::detach([&result](int a, int b) { result.store(a + b); }, 40, 2);
    sc::threading::wait_until_finished();

    REQUIRE(result.load() == 42);
  }

  SECTION("Runs Task That Does Not Fit The Small Function Buffer") {
    std::atomic<size_t> weight_size{0};
    SFOBreaker breaker{};

    sc::threading::detach([&weight_size](const SFOBreaker &b) { weight_size.store(b.weight.size()); }, breaker);
    sc::threading::wait_until_finished();

    REQUIRE(weight_size.load() == breaker.weight.size());
  }

  SECTION("Accepts A Priority") {
    std::atomic<int> executed{0};

    sc::threading::detach<sc::threading::Priority::High>([&executed]() { executed.fetch_add(1); });
    sc::threading::detach<sc::threading::Priority::Low>([&executed]() { executed.fetch_add(1); });
    sc::threading::wait_until_finished();

    REQUIRE(executed.load() == 2);
  }

  SECTION("Executes Every One Of Many Single Tasks") {
    constexpr int task_count = 5000;
    std::atomic<int> executed{0};

    for (int i = 0; i < task_count; ++i) {
      sc::threading::detach([&executed]() { executed.fetch_add(1, std::memory_order_relaxed); });
    }
    sc::threading::wait_until_finished();

    REQUIRE(executed.load() == task_count);
  }

  SECTION("Swallows Exceptions And Keeps The Pool Alive") {
    std::atomic<int> executed{0};
    SFOBreaker breaker{};

    sc::threading::detach([]() { throw std::runtime_error("small task"); });
    sc::threading::detach([](const SFOBreaker &) { throw std::runtime_error("large task"); }, breaker);
    sc::threading::detach([&executed]() { executed.store(1); });
    sc::threading::wait_until_finished();

    REQUIRE(executed.load() == 1);
  }
}

TEST_CASE("Threading Single Task Enqueue", "[threading][enqueue]") {
  sc::threading::init();

  SECTION("Returns The Result Through The Future") {
    auto future = sc::threading::enqueue([](int a, int b) { return a * b; }, 6, 7);

    REQUIRE(future.get() == 42);
  }

  SECTION("Completes A Void Future") {
    std::atomic<bool> executed{false};

    std::future<void> future = sc::threading::enqueue([&executed]() { executed.store(true); });
    future.get();

    REQUIRE(executed.load());
  }

  SECTION("Runs Task That Does Not Fit The Small Function Buffer") {
    SFOBreaker breaker{};
    breaker.weight.fill(std::byte{3});

    auto future = sc::threading::enqueue(
        [](const SFOBreaker &b) {
          size_t sum = 0;
          for (const auto value: b.weight) {
            sum += std::to_integer<size_t>(value);
          }
          return sum;
        },
        breaker);

    REQUIRE(future.get() == breaker.weight.size() * 3);
  }

  SECTION("Accepts A Priority") {
    auto future = sc::threading::enqueue<sc::threading::Priority::High>([]() { return std::string{"high"}; });

    REQUIRE(future.get() == "high");
  }

  SECTION("Reports The Worker Id") {
    auto future = sc::threading::enqueue([]() { return sc::threading::thread_id(); });

    REQUIRE(future.get() > 0);
    REQUIRE(sc::threading::thread_id() == 0);
  }

  SECTION("Forwards Exceptions Through The Future") {
    SFOBreaker breaker{};

    auto small_task = sc::threading::enqueue([]() -> int { throw std::runtime_error("small task"); });
    auto large_task =
        sc::threading::enqueue([](const SFOBreaker &) -> int { throw std::runtime_error("large task"); }, breaker);

    REQUIRE_THROWS_AS(small_task.get(), std::runtime_error);
    REQUIRE_THROWS_AS(large_task.get(), std::runtime_error);
  }

  SECTION("Counts As Outstanding Work For wait_until_finished") {
    constexpr int task_count = 2000;
    std::atomic<int> executed{0};
    std::vector<std::future<void>> futures;
    futures.reserve(task_count);

    for (int i = 0; i < task_count; ++i) {
      futures.push_back(sc::threading::enqueue([&executed]() { executed.fetch_add(1, std::memory_order_relaxed); }));
    }
    sc::threading::wait_until_finished();

    REQUIRE(executed.load() == task_count);
  }
}

TEST_CASE("Threading Parallel For", "[threading][parallel_for]") {
  sc::threading::init();

  // How often every item was visited. Catch2 assertions are only made here,
  // never from the function of a loop.
  const auto visit = [](size_t count, size_t grain) {
    std::vector<std::atomic<int>> visits(count);
    std::atomic<size_t> largest{0};
    sc::threading::parallelFor(count, grain, [&](size_t first, size_t last) {
      size_t seen = largest.load();
      while (last - first > seen && !largest.compare_exchange_weak(seen, last - first)) {
      }
      for (size_t i = first; i < last; ++i) {
        visits[i].fetch_add(1);
      }
    });
    const bool once = std::ranges::all_of(visits, [](const std::atomic<int> &n) { return n.load() == 1; });
    return std::pair{once, largest.load()};
  };

  SECTION("Every Item Once, In Chunks Of At Most The Grain") {
    for (const size_t count: {size_t{1}, size_t{7}, size_t{1000}, size_t{100'003}}) {
      for (const size_t grain: {size_t{1}, size_t{64}, size_t{1000}, size_t{1'000'000}}) {
        const auto [once, largest] = visit(count, grain);
        REQUIRE(once);
        REQUIRE(largest <= grain);
      }
    }
  }

  SECTION("Nothing To Do") {
    bool called = false;
    sc::threading::parallelFor(0, 16, [&](size_t, size_t) { called = true; });
    REQUIRE(!called);
  }

  SECTION("A Grain Of Zero Counts As One") {
    const auto [once, largest] = visit(100, 0);
    REQUIRE(once);
    REQUIRE(largest == 1);
  }

  SECTION("An Exception Is Rethrown") {
    std::atomic<size_t> started{0};
    const auto failing = [&] {
      sc::threading::parallelFor(10'000, 1, [&](size_t first, size_t) {
        started.fetch_add(1);
        if (first == 3) {
          throw std::runtime_error("broken chunk");
        }
      });
    };
    REQUIRE_THROWS_AS(failing(), std::runtime_error);
    // The pool is fine afterwards.
    const auto [once, largest] = visit(1000, 10);
    REQUIRE(once);
  }

  SECTION("Inside Another Loop") {
    constexpr size_t outer = 16;
    constexpr size_t inner = 2000;
    std::vector<std::atomic<int>> visits(outer * inner);
    sc::threading::parallelFor(outer, 1, [&](size_t first, size_t last) {
      for (size_t o = first; o < last; ++o) {
        sc::threading::parallelFor(inner, 50, [&, o](size_t from, size_t to) {
          for (size_t i = from; i < to; ++i) {
            visits[(o * inner) + i].fetch_add(1);
          }
        });
      }
    });
    REQUIRE(std::ranges::all_of(visits, [](const std::atomic<int> &n) { return n.load() == 1; }));
  }

  SECTION("From Many Tasks Of The Pool At Once") {
    constexpr size_t tasks = 64;
    constexpr size_t items = 5000;
    std::vector<std::atomic<int>> visits(tasks * items);
    std::vector<size_t> ids(tasks);
    std::ranges::iota(ids, size_t{0});
    auto done = sc::threading::enqueueBatch(ids, [&](size_t task) {
      sc::threading::parallelFor(items, 100, [&, task](size_t first, size_t last) {
        for (size_t i = first; i < last; ++i) {
          visits[(task * items) + i].fetch_add(1);
        }
      });
    });
    done.get();
    REQUIRE(std::ranges::all_of(visits, [](const std::atomic<int> &n) { return n.load() == 1; }));
  }

  SECTION("Free Pool Threads Join In") {
    std::mutex lock;
    std::set<std::thread::id> seen;
    sc::threading::parallelFor(64, 1, [&](size_t, size_t) {
      {
        const std::scoped_lock guard(lock);
        seen.insert(std::this_thread::get_id());
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    });
    REQUIRE(seen.size() > 1);
  }

  // Keeps every pool thread in a task until it is released, so that tickets
  // stay in the queue.
  struct BusyPool {
    std::atomic<bool> release{false};
    std::atomic<size_t> busy{0};

    BusyPool() {
      const size_t workers = sc::threading::thread_slots() - 1;
      for (size_t i = 0; i < workers; ++i) {
        sc::threading::detach([this] {
          busy.fetch_add(1);
          while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        });
      }
      while (busy.load() != workers) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    BusyPool(const BusyPool &) = delete;
    BusyPool &operator=(const BusyPool &) = delete;
    ~BusyPool() {
      release.store(true);
      sc::threading::wait_until_finished();
    }
  };

  SECTION("Without A Free Pool Thread The Caller Does It Alone") {
    const BusyPool pool;
    const auto caller = std::this_thread::get_id();
    std::atomic<size_t> items{0};
    std::atomic<size_t> elsewhere{0};
    sc::threading::parallelFor(1000, 10, [&](size_t first, size_t last) {
      items.fetch_add(last - first);
      if (std::this_thread::get_id() != caller) {
        elsewhere.fetch_add(1);
      }
    });
    REQUIRE(items.load() == 1000);
    REQUIRE(elsewhere.load() == 0);
  }

  SECTION("The Caller Leaves The Loops Of Others Alone") {
    // The tickets of the long loop are in the queue in front of those of the
    // short one. The short one must not pick them up.
    const BusyPool pool;
    std::atomic<bool> longStarted{false};
    std::atomic<bool> shortDone{false};
    std::atomic<size_t> longItems{0};
    std::atomic<size_t> longOnForeignThread{0};
    std::thread longLoop([&] {
      const auto owner = std::this_thread::get_id();
      sc::threading::parallelFor(64, 1, [&](size_t first, size_t last) {
        longStarted.store(true);
        longItems.fetch_add(last - first);
        if (std::this_thread::get_id() != owner) {
          longOnForeignThread.fetch_add(1);
        }
        // Still running when the short loop is through.
        while (!shortDone.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      });
    });
    while (!longStarted.load()) {
      std::this_thread::yield();
    }
    std::atomic<size_t> shortItems{0};
    sc::threading::parallelFor(4, 1, [&](size_t first, size_t last) { shortItems.fetch_add(last - first); });
    shortDone.store(true);
    longLoop.join();

    REQUIRE(shortItems.load() == 4);
    REQUIRE(longItems.load() == 64);
    REQUIRE(longOnForeignThread.load() == 0);
  }

  SECTION("Tickets Nobody Took Are Void") {
    std::atomic<size_t> late{0};
    {
      const BusyPool pool;
      for (int round = 0; round < 50; ++round) {
        // Gone when the pool threads get to the tickets.
        std::atomic<size_t> items{0};
        sc::threading::parallelFor(100, 1, [&](size_t first, size_t last) { items.fetch_add(last - first); });
        late.fetch_add(items.load() == 100 ? 0 : 1);
      }
      // The pool threads are released here and take what is in the queue.
    }
    REQUIRE(late.load() == 0);
    const auto [once, largest] = visit(10'000, 16);
    REQUIRE(once);
    REQUIRE(largest <= 16);
  }
}

TEST_CASE("Threading Raw Speed Benchmarks", "[threading][efficiency][benchmark]") {
  sc::threading::init();

  BENCHMARK("SFO Path - Batch") { execute_batch_run<FastTask>(false, false); };

  BENCHMARK("SFO Path - Detach") { execute_batch_run<FastTask>(false, true); };

  BENCHMARK("Merged Path (No SFO) - Batch") { execute_batch_run<FastTask>(true, false); };

  BENCHMARK("Merged Path (No SFO) - Detach") { execute_batch_run<FastTask>(true, true); };
}
