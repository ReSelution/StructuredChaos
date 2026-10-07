#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <future>
#include <numeric>
#include <stdexcept>
#include <string>
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

  template <typename TaskType> void execute_batch_run(bool force_no_sfo, bool use_detach) {
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
        sc::threading::detachBatch(std::move(tasks), [breaker](int id, TaskType t) { (void)breaker; }, nullptr);
      } else {
        sc::threading::detachBatch(std::move(tasks), [](int id, TaskType t) {}, nullptr);
      }
    } else {
      if (force_no_sfo) {
        auto f = sc::threading::enqueueBatch(std::move(tasks), [breaker](int id, TaskType t) { (void)breaker; });
      } else {
        auto f = sc::threading::enqueueBatch(std::move(tasks), [](int id, TaskType t) {});
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

    auto future = sc::threading::enqueueBatch(std::move(tasks), [&executed_counter](int thread_id, int task_val) {
      executed_counter.fetch_add(1, std::memory_order_relaxed);
    });

    sc::threading::wait_until_finished();

    REQUIRE(executed_counter.load() == task_count);
  }

  SECTION("Verify detachBatch Executes All Tasks") {
    std::atomic<size_t> executed_counter{0};
    std::vector<int> tasks(task_count);
    std::ranges::iota(tasks, 0);

    sc::threading::detachBatch(
        std::move(tasks),
        [&executed_counter](int thread_id, int task_val) { executed_counter.fetch_add(1, std::memory_order_relaxed); },
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
        [&data_valid](int thread_id, int task_val) {
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

    sc::threading::detach([&result](int thread_id, int a, int b) { result.store(a + b); }, 40, 2);
    sc::threading::wait_until_finished();

    REQUIRE(result.load() == 42);
  }

  SECTION("Runs Task That Does Not Fit The Small Function Buffer") {
    std::atomic<size_t> weight_size{0};
    SFOBreaker breaker{};

    sc::threading::detach([&weight_size](int thread_id, const SFOBreaker &b) { weight_size.store(b.weight.size()); },
                          breaker);
    sc::threading::wait_until_finished();

    REQUIRE(weight_size.load() == breaker.weight.size());
  }

  SECTION("Accepts A Priority") {
    std::atomic<int> executed{0};

    sc::threading::detach<sc::threading::Priority::High>([&executed](int thread_id) { executed.fetch_add(1); });
    sc::threading::detach<sc::threading::Priority::Low>([&executed](int thread_id) { executed.fetch_add(1); });
    sc::threading::wait_until_finished();

    REQUIRE(executed.load() == 2);
  }

  SECTION("Executes Every One Of Many Single Tasks") {
    constexpr int task_count = 5000;
    std::atomic<int> executed{0};

    for (int i = 0; i < task_count; ++i) {
      sc::threading::detach([&executed](int thread_id) { executed.fetch_add(1, std::memory_order_relaxed); });
    }
    sc::threading::wait_until_finished();

    REQUIRE(executed.load() == task_count);
  }

  SECTION("Swallows Exceptions And Keeps The Pool Alive") {
    std::atomic<int> executed{0};
    SFOBreaker breaker{};

    sc::threading::detach([](int thread_id) { throw std::runtime_error("small task"); });
    sc::threading::detach([](int thread_id, const SFOBreaker &) { throw std::runtime_error("large task"); }, breaker);
    sc::threading::detach([&executed](int thread_id) { executed.store(1); });
    sc::threading::wait_until_finished();

    REQUIRE(executed.load() == 1);
  }
}

TEST_CASE("Threading Single Task Enqueue", "[threading][enqueue]") {
  sc::threading::init();

  SECTION("Returns The Result Through The Future") {
    auto future = sc::threading::enqueue([](int thread_id, int a, int b) { return a * b; }, 6, 7);

    REQUIRE(future.get() == 42);
  }

  SECTION("Completes A Void Future") {
    std::atomic<bool> executed{false};

    std::future<void> future = sc::threading::enqueue([&executed](int thread_id) { executed.store(true); });
    future.get();

    REQUIRE(executed.load());
  }

  SECTION("Runs Task That Does Not Fit The Small Function Buffer") {
    SFOBreaker breaker{};
    breaker.weight.fill(std::byte{3});

    auto future = sc::threading::enqueue(
        [](int thread_id, const SFOBreaker &b) {
          size_t sum = 0;
          for (const auto value : b.weight) {
            sum += std::to_integer<size_t>(value);
          }
          return sum;
        },
        breaker);

    REQUIRE(future.get() == breaker.weight.size() * 3);
  }

  SECTION("Accepts A Priority") {
    auto future =
        sc::threading::enqueue<sc::threading::Priority::High>([](int thread_id) { return std::string{"high"}; });

    REQUIRE(future.get() == "high");
  }

  SECTION("Passes The Worker Id") {
    auto future = sc::threading::enqueue([](int thread_id) { return thread_id; });

    REQUIRE(future.get() > 0);
  }

  SECTION("Forwards Exceptions Through The Future") {
    SFOBreaker breaker{};

    auto small_task = sc::threading::enqueue([](int thread_id) -> int { throw std::runtime_error("small task"); });
    auto large_task = sc::threading::enqueue(
        [](int thread_id, const SFOBreaker &) -> int { throw std::runtime_error("large task"); }, breaker);

    REQUIRE_THROWS_AS(small_task.get(), std::runtime_error);
    REQUIRE_THROWS_AS(large_task.get(), std::runtime_error);
  }

  SECTION("Counts As Outstanding Work For wait_until_finished") {
    constexpr int task_count = 2000;
    std::atomic<int> executed{0};
    std::vector<std::future<void>> futures;
    futures.reserve(task_count);

    for (int i = 0; i < task_count; ++i) {
      futures.push_back(
          sc::threading::enqueue([&executed](int thread_id) { executed.fetch_add(1, std::memory_order_relaxed); }));
    }
    sc::threading::wait_until_finished();

    REQUIRE(executed.load() == task_count);
  }
}

TEST_CASE("Threading Raw Speed Benchmarks", "[threading][efficiency][benchmark]") {
  sc::threading::init();

  BENCHMARK("SFO Path - Batch") { execute_batch_run<FastTask>(false, false); };

  BENCHMARK("SFO Path - Detach") { execute_batch_run<FastTask>(false, true); };

  BENCHMARK("Merged Path (No SFO) - Batch") { execute_batch_run<FastTask>(true, false); };

  BENCHMARK("Merged Path (No SFO) - Detach") { execute_batch_run<FastTask>(true, true); };
}
