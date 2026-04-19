#include <iostream>
#include <vector>
#include <string>
#include <cassert>

#include "threading/chaos_threading.hpp"
#include "stats/chaos_stats.hpp"

// Logging Aliases
LOG_ALIAS(StressLog, "Chaos", "StressTest");

// Statistics Definitions
DEFINE_CHAOS_CORE_STAT(BatchThroughput, "Batch Processing", SC::ChaosThroughput<SC::MetricUnits>);

// --- Test Components ---

struct SFOBreaker {
  std::array<std::byte, 100> weight;
};

// Für die Verifikation (langsam wegen atomics)
struct TrackedTask {
  inline static std::atomic<int> copies{0};
  inline static std::atomic<int> moves{0};

  int id;

  TrackedTask(int i) : id(i) {
  }

  TrackedTask(const TrackedTask &o) : id(o.id) { copies++; }
  TrackedTask(TrackedTask &&o) noexcept : id(o.id) { moves++; }
};

// Für den echten Speed-Test (Maximum Warp)
struct FastTask {
  int id;

  FastTask(int i) : id(i) {
  }

  // Keine Counter, keine Seiteneffekte -> Compiler-Himmel
};

template<typename TaskType>
void run_efficiency_block(bool track_stats) {
  StressLog::info("--- Efficiency Test (Mode: {}) ---", track_stats ? "VERIFICATION" : "RAW_SPEED");

  auto run_test = [track_stats](std::string_view mode_name, bool force_no_sfo, bool use_detach) {
    if constexpr (std::is_same_v<TaskType, TrackedTask>) {
      TrackedTask::copies = 0;
      TrackedTask::moves = 0;
    }

    constexpr size_t TASKS = 20000;
    std::vector<TaskType> tasks;
    tasks.reserve(TASKS);
    for (int i = 0; i < TASKS; ++i) tasks.emplace_back(i);

    SFOBreaker breaker;
    std::string full_mode_name = std::string(mode_name) + (use_detach ? "_DETACH" : "_BATCH");

    {
      BatchThroughput::reset();
      BatchThroughput::start();

      SC::PoolThroughput::m_storage.value.store(0, std::memory_order_relaxed);
      SC::PoolThroughput::m_storage.accumulated_ns.store(0, std::memory_order_relaxed);
      SC::PoolThroughput::m_storage.running.store(false, std::memory_order_relaxed);
      SC::PoolThroughput::start();
      auto t = StressLog::time("Mode: {1} | SFO: {2} took {0}",
                               full_mode_name, !force_no_sfo);

      if (use_detach) {
        if (force_no_sfo) {
          SC::ChaosThreading::detacheBatch(std::move(tasks),
                                           [breaker](int id, TaskType t) { (void) breaker; }, nullptr);
        } else {
          SC::ChaosThreading::detacheBatch(std::move(tasks),
                                           [](int id, TaskType t) {
                                           }, nullptr);
        }
      } else {
        if (force_no_sfo) {
          auto f = SC::ChaosThreading::enqueueBatch(std::move(tasks),
                                                    [breaker](int id, TaskType t) { (void) breaker; });
        } else {
          auto f = SC::ChaosThreading::enqueueBatch(std::move(tasks),
                                                    [](int id, TaskType t) {
                                                    });
        }
      }
      t.stop();

      // Stats Update: Wir tracken, wie viele Tasks wir gerade "gefeuert" haben
      BatchThroughput::record(TASKS);
      BatchThroughput::stop();
      SC::ChaosThreading::wait_until_finished();
      StressLog::stats<BatchThroughput, SC::PoolThroughput>("");
    }

    if constexpr (std::is_same_v<TaskType, TrackedTask>) {
      StressLog::info("  -> Results: Copies={}, Moves={}", (int) TrackedTask::copies, (int) TrackedTask::moves);
    }
  };

  run_test("SFO_PATH", false, false); // BATCH
  run_test("SFO_PATH", false, true); // DETACH
  run_test("MERGED_PATH", true, false); // BATCH
  run_test("MERGED_PATH", true, true); // DETACH

  // Ausgabe der Chaos-Stats für diesen Block
};

void test_efficiency_full() {
  // 1. Korrektheit prüfen (mit TrackedTask)
  run_efficiency_block<TrackedTask>(true);

  // 2. Rohe Performance messen (mit FastTask)
  run_efficiency_block<FastTask>(false);
}

int main() {
  try {
    SC::ChaosThreading::init();

    test_efficiency_full();

    StressLog::info("All Stress Tests and Efficiency Checks completed.");
  } catch (const std::exception &e) {
    StressLog::err("Test failed with exception: {}", e.what());
    return 1;
  }
  return 0;
}
