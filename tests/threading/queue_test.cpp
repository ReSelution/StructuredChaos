#include <atomic>
#include <cassert>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory> // Für std::unique_ptr
#include <thread>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#include <psapi.h>
#include <windows.h>
#else
#include <fstream>
#include <unistd.h>
#endif

#include "atomic_queue/atomic_queue.h"

// Eigene Module importieren
import sc.threading;
import sc.stats;
import sc.logger;

using StressLog       = sc::Logger<"Chaos", "MPMC_Stress">;
using TotalThroughput = sc::stats::Stat<"MPMC Sync Processing", sc::stats::Throughput<sc::stats::MetricUnits>>;
using namespace std::literals::chrono_literals;

using MoveOnlyFunction = std::move_only_function<void(int)>;

constexpr size_t OperationsPerThread = 100'000;
constexpr size_t QueueCapacity       = 1'048'576; // 2^20 Slots

[[nodiscard]] size_t get_current_rss_mb() noexcept {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return pmc.WorkingSetSize / (1024 * 1024);
  }
  return 0;
#else
  std::ifstream statm("/proc/self/statm");
  size_t size, resident;
  if (statm >> size >> resident) {
    return (resident * sysconf(_SC_PAGESIZE)) / (1024 * 1024);
  }
  return 0;
#endif
}

// Generischer Test-Runner für beliebige Queue-Typen
template<typename QueueType, typename ElementType>
void run_mpmc_test(const std::string &queueName, unsigned int producerCount, unsigned int consumerCount) {
  const size_t totalExpectedElements = producerCount * OperationsPerThread;

  // --- DIE ANPASSUNG FÜR DEN KONSTRUKTOR & HEAP-ALLOKATION ---
  std::unique_ptr<QueueType> queue_ptr;

  if constexpr (std::is_constructible_v<QueueType, size_t>) {
    // Falls die Queue (wie AtomicQueueB2) die Größe zur Laufzeit im Konstruktor will:
    queue_ptr = std::make_unique<QueueType>(QueueCapacity);
  } else {
    // Falls die Queue (wie deine eigene) die Größe als Template-Parameter hat und Standard-konstruierbar ist:
    queue_ptr = std::make_unique<QueueType>();
  }

  // Komfortable Referenz, damit der restliche Code unberührt bleibt
  QueueType &queue = *queue_ptr;

  std::atomic<long long> validationChecksum{0};
  std::atomic<size_t> totalConsumedCount{0};
  std::atomic<bool> startSignal{false};
  std::atomic<bool> producersFinished{false};

  StressLog::info("=========================================");
  StressLog::info("Starte Benchmark für: {}", queueName);
  StressLog::info("Producer: {} | Consumer: {} | Elemente gesamt: {}", producerCount, consumerCount,
                  totalExpectedElements);

  std::vector<std::jthread> workers;
  workers.reserve(producerCount + consumerCount);

  // --- 1. PRODUCER THREADS ---
  for (unsigned int t = 0; t < producerCount; ++t) {
    workers.emplace_back([&queue, &startSignal, t, &validationChecksum]() {
      while (!startSignal.load(std::memory_order_relaxed)) {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#endif
      }

      size_t startOffset         = t * OperationsPerThread;
      long long localChecksumSum = 0;

      for (size_t i = 0; i < OperationsPerThread; ++i) {
        size_t taskId = startOffset + i;
        localChecksumSum += static_cast<long long>(taskId);

        ElementType element = [taskId, &validationChecksum](int) {
          auto volatile dummy = taskId ^ 0x55AA55AA;
          validationChecksum.fetch_sub(static_cast<long long>(taskId), std::memory_order_relaxed);
        };

        if constexpr (requires { queue.push(std::move(element)); }) {
          queue.push(std::move(element));
        } else {
          while (!queue.try_push(std::move(element))) {
#if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
#endif
          }
        }
      }
      validationChecksum.fetch_add(localChecksumSum, std::memory_order_relaxed);
    });
  }

  // --- 2. CONSUMER THREADS ---
  for (unsigned int t = 0; t < consumerCount; ++t) {
    workers.emplace_back([&queue, &startSignal, &producersFinished, &totalConsumedCount]() {
      while (!startSignal.load(std::memory_order_relaxed)) {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#endif
      }

      ElementType func;
      size_t localConsumed = 0;

      while (true) {
        bool popSuccess = false;
        if constexpr (requires { queue.try_pop(func); }) {
          popSuccess = queue.try_pop(func);
        } else if constexpr (requires { queue.pop(func); }) {
          popSuccess = queue.pop(func);
        }

        if (popSuccess) {
          func(0);
          localConsumed++;
          continue;
        }

        if (producersFinished.load(std::memory_order_acquire)) {
          bool finalPop = false;
          if constexpr (requires { queue.try_pop(func); }) {
            finalPop = queue.try_pop(func);
          } else if constexpr (requires { queue.pop(func); }) {
            finalPop = queue.pop(func);
          }

          if (!finalPop) {
            break;
          }
          func(0);
          localConsumed++;
          continue;
        }

#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#endif
      }

      totalConsumedCount.fetch_add(localConsumed, std::memory_order_relaxed);
    });
  }

  // --- 3. START UND ZEITMESSUNG ---
  size_t initialRam = get_current_rss_mb();
  StressLog::info("RAM vor dem Startschuss: {} MB Stack: {} B", initialRam, sizeof(QueueType));


  TotalThroughput::reset();
  TotalThroughput::start();

  startSignal.store(true, std::memory_order_release);

  for (size_t i = 0; i < producerCount; ++i) {
    workers[i].join();
  }

  producersFinished.store(true, std::memory_order_release);

  for (size_t i = producerCount; i < workers.size(); ++i) {
    workers[i].join();
  }

  TotalThroughput::record(totalExpectedElements);
  TotalThroughput::stop();
  std::this_thread::sleep_for(10ms);
  size_t finalRam = get_current_rss_mb();

  // --- 4. BEWERTUNG ---
  StressLog::stats<TotalThroughput>("{}", queueName);
  StressLog::info("RAM nach Abschluss: {} MB", finalRam);

  assert(totalConsumedCount.load() == totalExpectedElements && "CRITICAL: Datenverlust oder Duplikate!");
  assert(validationChecksum.load() == 0 && "CRITICAL: Datenkorruption!");
  StressLog::info("✅ {} Test fehlerfrei bestanden!", queueName);
}

int main() {
  const unsigned int halfHardware  = std::thread::hardware_concurrency() / 2;
  const unsigned int producerCount = halfHardware > 1 ? halfHardware : 2;
  const unsigned int consumerCount = producerCount;

  // BENCHMARK 1: Deine eigene Implementierung (Größe im Template)
  using MyQueueType = sc::AtomicQueue<MoveOnlyFunction, QueueCapacity>;
  run_mpmc_test<MyQueueType, MoveOnlyFunction>("SC::AtomicQueue", producerCount, consumerCount);

  // BENCHMARK 2: Die externe atomic_queue (Größe im Konstruktor via B2-Variante)
  using ExternalQueueType = atomic_queue::AtomicQueueB2<MoveOnlyFunction, std::allocator<MoveOnlyFunction>>;
  run_mpmc_test<ExternalQueueType, MoveOnlyFunction>("atomic_queue::AtomicQueueB2", producerCount, consumerCount);

  return 0;
}
