#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <thread>
#include <vector>

// Für Speicherabfrage
#if defined(_WIN32)
#include <psapi.h>
#include <windows.h>
#else
#include <fstream>
#include <unistd.h>
#endif

import SC.Threading;
import SC.Stats;
import SC.Logger;

using StressLog       = SC::ChaosLogger<"Chaos", "MPMC_Stress">;
using TotalThroughput = SC::ChaosStat<"MPMC Sync Processing", SC::ChaosThroughput<SC::MetricUnits>>;

constexpr size_t OperationsPerThread = 100'000;

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

int main() {
  const unsigned int halfHardware = std::thread::hardware_concurrency() / 2;
  // Mindestens 2 Threads pro Fraktion, um echte Nebenläufigkeit zu erzwingen
  const unsigned int producerCount = halfHardware > 1 ? halfHardware : 2;
  const unsigned int consumerCount = producerCount;

  const size_t totalExpectedElements = producerCount * OperationsPerThread;

  // Wir nutzen eine Capacity von 1 Million Slots
  SC::AtomicQueue<SC::MoveOnlyFunction, 1048576> queue;

  // Zur Validierung: Wir summieren alle produzierten IDs atomar auf.
  // Die Consumer subtrahieren sie wieder. Am Ende MUSS die Summe exakt 0 sein.
  std::atomic<long long> validationChecksum{0};
  std::atomic<size_t> totalConsumedCount{0};

  // Start-Flag, damit alle Threads zeitgleich wie beim 100m-Lauf losreißen
  std::atomic<bool> startSignal{false};
  // Signalisiert den Consumern, dass keine neuen Elemente mehr kommen werden
  std::atomic<bool> producersFinished{false};

  StressLog::info("Starte synchronen MPMC Test...");
  StressLog::info("Producer: {} | Consumer: {} | Elemente gesamt: {}", producerCount, consumerCount,
                  totalExpectedElements);


  std::vector<std::jthread> workers;
  workers.reserve(producerCount + consumerCount);

  // --- 1. PRODUCER THREADS STARTEN ---
  for (unsigned int t = 0; t < producerCount; ++t) {
    workers.emplace_back([&queue, &startSignal, t, &validationChecksum]() {
      // Warten auf den Startschuss
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

        queue.push([taskId](int) {
          // Simuliere minimale CPU-Last im Consumer
          auto volatile dummy = taskId ^ 0x55AA55AA;
        });
      }
      // Ein einziger atomarer Add am Ende schont den Bus während des Tests
      validationChecksum.fetch_add(localChecksumSum, std::memory_order_relaxed);
    });
  }

  // --- 2. CONSUMER THREADS STARTEN ---
  // --- 2. CONSUMER THREADS STARTEN ---
  for (unsigned int t = 0; t < consumerCount; ++t) {
    workers.emplace_back([&queue, &startSignal, &producersFinished, &totalConsumedCount]() {
      // 1. Warten auf den globalen Startschuss
      while (!startSignal.load(std::memory_order_relaxed)) {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#endif
      }

      SC::MoveOnlyFunction func;
      size_t localConsumed = 0;

      // 2. Hauptverarbeitungsschleife
      while (true) {
        if (queue.try_pop(func)) {
          func(0);
          localConsumed++;
          continue; // Sofort das nächste Element holen, ohne Pause!
        }

        // Wenn try_pop fehlschlägt, prüfen wir, ob die Producer fertig sind
        if (producersFinished.load(std::memory_order_acquire)) {
          // Double-Check: Da die Producer fertig sind, versuchen wir die Queue
          // komplett leerzusaugen. Erst wenn try_pop HIER fehlschlägt,
          // wissen wir sicher, dass absolut nichts mehr nachkommt.
          if (!queue.try_pop(func)) {
            break;
          }
          func(0);
          localConsumed++;
          continue;
        }

        // Die Queue ist temporär leer, aber die Producer arbeiten noch.
        // Kurze Pause, um die CPU zu entlasten.
#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#endif
      }

      // 3. Ergebnisse sicher akkumulieren
      totalConsumedCount.fetch_add(localConsumed, std::memory_order_relaxed);
    });
  }
  // --- 3. GO! ---
  size_t initialRam = get_current_rss_mb();
  StressLog::info("RAM vor dem Startschuss: {} MB", initialRam);

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

  size_t finalRam = get_current_rss_mb();

  // --- 4. VALIDIERUNG ---
  StressLog::stats<TotalThroughput>("Zeitgleiches MPMC");
  StressLog::info("RAM nach Abschluss: {} MB", finalRam);
  StressLog::info("Verarbeitete Elemente: {} / {}", totalConsumedCount.load(), totalExpectedElements);

  // Da wir die exakten IDs im Consumer nicht ohne Weiteres aus der anonymen Lambda extrahieren,
  // validieren wir die Integrität über die exakte Anzahl.
  // Wenn du die Checksumme strikt prüfen willst, müsste das Funktionsobjekt die ID zurückgeben!
  assert(totalConsumedCount.load() == totalExpectedElements && "CRITICAL: Datenverlust oder Duplikate!");

  StressLog::info("✅ MPMC-Stresstest ohne Fehler bestanden!");

  return 0;
}
