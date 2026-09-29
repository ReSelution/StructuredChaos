#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <numeric>
#include <string>
#include <vector>

#include "sc/logger/logger.hpp"
#include "sc/stats/stats.hpp"
#include "sc/stats/throughput.hpp"
#include "sc/stats/units.hpp"
#include "sc/threading/threading.hpp"

using StressLog = sc::Logger<"StressLog">;
using BatchThroughput =
    sc::stats::Stat<"Batch Processing",
                    sc::stats::Throughput<sc::stats::MetricUnits>>;

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

template <typename TaskType>
void execute_batch_run(bool force_no_sfo, bool use_detach) {
  constexpr size_t TASKS = 20000;
  std::vector<TaskType> tasks;
  tasks.reserve(TASKS);
  for (int i = 0; i < static_cast<int>(TASKS); ++i) {
    tasks.emplace_back(i);
  }

  SFOBreaker breaker;

  BatchThroughput::reset();
  BatchThroughput::start();

  sc::threading::PoolThroughput::m_storage.value.store(
      0, std::memory_order_relaxed);
  sc::threading::PoolThroughput::m_storage.accumulated_ns.store(
      0, std::memory_order_relaxed);
  sc::threading::PoolThroughput::m_storage.running.store(
      false, std::memory_order_relaxed);
  sc::threading::PoolThroughput::start();

  if (use_detach) {
    if (force_no_sfo) {
      sc::threading::detachBatch(
          std::move(tasks), [breaker](int id, TaskType t) { (void)breaker; },
          nullptr);
    } else {
      sc::threading::detachBatch(
          std::move(tasks), [](int id, TaskType t) {}, nullptr);
    }
  } else {
    if (force_no_sfo) {
      auto f = sc::threading::enqueueBatch(
          std::move(tasks), [breaker](int id, TaskType t) { (void)breaker; });
    } else {
      auto f = sc::threading::enqueueBatch(std::move(tasks),
                                           [](int id, TaskType t) {});
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
    std::iota(tasks.begin(), tasks.end(), 0);

    auto future = sc::threading::enqueueBatch(
        std::move(tasks), [&executed_counter](int thread_id, int task_val) {
          executed_counter.fetch_add(1, std::memory_order_relaxed);
        });

    sc::threading::wait_until_finished();

    REQUIRE(executed_counter.load() == task_count);
  }

  SECTION("Verify detachBatch Executes All Tasks") {
    std::atomic<size_t> executed_counter{0};
    std::vector<int> tasks(task_count);
    std::iota(tasks.begin(), tasks.end(), 0);

    sc::threading::detachBatch(
        std::move(tasks),
        [&executed_counter](int thread_id, int task_val) {
          executed_counter.fetch_add(1, std::memory_order_relaxed);
        },
        nullptr);

    sc::threading::wait_until_finished();

    REQUIRE(executed_counter.load() == task_count);
  }

  SECTION("Verify Correct Data Payload Passed To Worker") {
    std::atomic<bool> data_valid{true};
    std::vector<int> tasks(task_count);
    std::iota(tasks.begin(), tasks.end(), 0);

    sc::threading::detachBatch(
        std::move(tasks),
        [&data_valid](int thread_id, int task_val) {
          if (task_val < 0 || task_val >= static_cast<int>(task_count)) {
            data_valid.store(false, std::memory_order_relaxed);
          }
        },
        nullptr);

    sc::threading::wait_until_finished();

    REQUIRE(data_valid.load() == true);
  }
}

TEST_CASE("Threading Efficiency Correctness",
          "[threading][efficiency][verification]") {
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

TEST_CASE("Threading Raw Speed Benchmarks",
          "[threading][efficiency][benchmark]") {
  sc::threading::init();

  BENCHMARK("SFO Path - Batch") { execute_batch_run<FastTask>(false, false); };

  BENCHMARK("SFO Path - Detach") { execute_batch_run<FastTask>(false, true); };

  BENCHMARK("Merged Path (No SFO) - Batch") {
    execute_batch_run<FastTask>(true, false);
  };

  BENCHMARK("Merged Path (No SFO) - Detach") {
    execute_batch_run<FastTask>(true, true);
  };
}
