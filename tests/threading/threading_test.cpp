#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <glm/glm.hpp>

#include "threading/chaos_threading.hpp"
#include "ecs/chaos_registry.hpp"
#include "stats/chaos_counter.hpp"

// Logging Aliases
LOG_ALIAS(StressLog, "Chaos", "StressTest");

// Statistics Definitions
DEFINE_CHAOS_CORE_STAT(BatchThroughput, "Batch Processing", SC::ChaosThroughput<SC::MetricUnits>);
DEFINE_CHAOS_CORE_STAT(MoveEfficiency, "Move Operations", SC::ChaosCounter<>);

// --- Test Components ---

struct SFOBreaker {
  std::array<std::byte, 100> weight;
};

struct TrackedTask {
  inline static std::atomic<int> copies{0};
  inline static std::atomic<int> moves{0};

  int id;
  TrackedTask(int i) : id(i) {}
  TrackedTask(const TrackedTask& o) : id(o.id) { copies++; }
  TrackedTask(TrackedTask&& o) noexcept : id(o.id) { moves++; }
};


void test_efficiency_full() {
  StressLog::info("Starting Efficiency Test: SFO vs. Merged Storage...");

  auto run_test = [](std::string_view mode_name, bool use_move, bool force_no_sfo) {
    TrackedTask::copies = 0;
    TrackedTask::moves = 0;

    constexpr size_t TASKS = 200;
    std::vector<TrackedTask> tasks;
    tasks.reserve(TASKS);
    for(int i = 0; i < TASKS; ++i) tasks.emplace_back(i);

    // Falls wir SFO umgehen wollen, capturen wir dieses Objekt
    SFOBreaker breaker;

    {
      auto t = StressLog::time("Mode: {1} | Move: {2} | SFO: {3} took {0}" ,
                               mode_name, use_move, !force_no_sfo);

      if (force_no_sfo) {
        // Lambda ist hier > 100 Bytes -> Merged Storage wird erzwungen
        if (use_move) {
          auto f = SC::ChaosThreading::enqueueBatch(std::move(tasks),
                                                    [breaker](int id, TrackedTask t) { (void)breaker; });
          t.stop();
          f.get();
        } else {
          auto f = SC::ChaosThreading::enqueueBatch(tasks,
                                                    [breaker](int id, TrackedTask t) { (void)breaker; });
          t.stop();
          f.get();
        }
      } else {
        // Normaler Pfad (SFO falls möglich)
        if (use_move) {
          auto f = SC::ChaosThreading::enqueueBatch(std::move(tasks),
                                                    [](int id, TrackedTask t) {});
          t.stop();
          f.get();
        } else {
          auto f = SC::ChaosThreading::enqueueBatch(tasks,
                                                    [](int id, TrackedTask t) {});
          t.stop();
          f.get();
        }
      }
    }

    StressLog::info("Result [{}]: Copies={}, Moves={}", mode_name, (int)TrackedTask::copies, (int)TrackedTask::moves);
  };

  // 1. SFO Pfad (Standard)
  run_test("SFO_PATH", false, false);
  run_test("SFO_PATH", true, false);

  // 2. Merged Storage Pfad (Breaker erzwingt Case 2)
  run_test("MERGED_STORAGE_PATH", false, true);
  run_test("MERGED_STORAGE_PATH", true, true);
}

int main() {
  try {
    SC::ChaosThreading::init();

    // Correctness & Efficiency Test
    test_efficiency_full();

    StressLog::info("All Stress Tests and Efficiency Checks completed.");
  } catch (const std::exception& e) {
    StressLog::err("Test failed with exception: {}", e.what());
    return 1;
  }
  return 0;
}