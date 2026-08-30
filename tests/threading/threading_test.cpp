#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Die neuen C++20 Module importieren
import sc.threading;
import sc.stats;
import sc.logger;
using StressLog = sc::Logger<"Chaos", "StressTest">;
// ============================================================================
// C++20 Stats Definition
// ============================================================================
using BatchThroughput = sc::stats::Stat<"Batch Processing", sc::stats::Throughput<sc::stats::MetricUnits>>;

// --- Test Components ---

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
};

struct FastTask {
  int id;
  FastTask(int i) : id(i) {}
};

template<typename TaskType>
void run_efficiency_block(bool track_stats) {
  StressLog::info("--- Efficiency Test (Mode: {}) ---", track_stats ? "VERIFICATION" : "RAW_SPEED");

  auto run_test = [](std::string_view mode_name, bool force_no_sfo, bool use_detach) {
    if constexpr (std::is_same_v<TaskType, TrackedTask>) {
      TrackedTask::copies = 0;
      TrackedTask::moves  = 0;
    }

    constexpr size_t TASKS = 20000;
    std::vector<TaskType> tasks;
    tasks.reserve(TASKS);
    for (int i = 0; i < TASKS; ++i)
      tasks.emplace_back(i);

    SFOBreaker breaker;
    std::string full_mode_name = std::string(mode_name) + (use_detach ? "_DETACH" : "_BATCH");

    {

      BatchThroughput::reset();
      BatchThroughput::start();

      sc::threading::PoolThroughput::m_storage.value.store(0, std::memory_order_relaxed);
      sc::threading::PoolThroughput::m_storage.accumulated_ns.store(0, std::memory_order_relaxed);
      sc::threading::PoolThroughput::m_storage.running.store(false, std::memory_order_relaxed);
      sc::threading::PoolThroughput::start();
      auto t = StressLog::time("Mode: {1} | SFO: {2} took {0}", full_mode_name, force_no_sfo);

      if (use_detach) {
        if (force_no_sfo) {
          sc::threading::detachBatch(std::move(tasks), [breaker](int id, TaskType t) { (void) breaker; }, nullptr);
        } else {
          sc::threading::detachBatch(std::move(tasks), [](int id, TaskType t) {}, nullptr);
        }
      } else {
        if (force_no_sfo) {
          auto f = sc::threading::enqueueBatch(std::move(tasks), [breaker](int id, TaskType t) { (void) breaker; });
        } else {
          auto f = sc::threading::enqueueBatch(std::move(tasks), [](int id, TaskType t) {});
        }
      }
      // RAII-Timer stoppt hier (oder implizit am Scope-Ende durch Destruktor)
    }

    BatchThroughput::record(TASKS);

    BatchThroughput::stop();
    sc::threading::wait_until_finished();

    // Nutzt deine neue, variadische Template-Variante für formatierte
    // Statistik-Ausgaben
    StressLog::stats<BatchThroughput, sc::threading::PoolThroughput>("Block finished");

    if constexpr (std::is_same_v<TaskType, TrackedTask>) {
      StressLog::info("  -> Results: Copies={}, Moves={}", TrackedTask::copies.load(std::memory_order_relaxed),
                      TrackedTask::moves.load(std::memory_order_relaxed));
    }
  };

  run_test("SFO_PATH", false, false); // BATCH
  run_test("SFO_PATH", false, true); // DETACH
  run_test("MERGED_PATH", true, false); // BATCH
  run_test("MERGED_PATH", true, true); // DETACH
}

void test_efficiency_full() {
  run_efficiency_block<TrackedTask>(true);
  run_efficiency_block<FastTask>(false);
}

int main() {
  try {
    // Initialisierung beider Subsysteme
    StressLog::init();
    sc::threading::init();

    test_efficiency_full();

    StressLog::info("All Stress Tests and Efficiency Checks completed.");

    StressLog::shutdown();
  } catch (const std::exception &e) {
    StressLog::err("Test failed with exception: {}", e.what());
    return 1;
  }
  return 0;
}
