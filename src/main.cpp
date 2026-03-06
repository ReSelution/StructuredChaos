#include "logger/chaos_logger.hpp"
#include "stats/chaos_global.hpp"
#include "threading/chaos_threading.hpp"

LOG_ALIAS(PoolLog, "Chaos", "ThreadPool");

int main() {
  SC::ChaosThreading::init();

  PoolLog::info("Starte Belastungstest für StructuredChaos Pool...");

  {
    auto t = PoolLog::time("Verarbeitung von 1000 Tasks dauerte {}");

    for (int i = 0; i < 1000; ++i) {
      SC::ChaosThreading::enqueue([]() {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      });
    }

    PoolLog::stats<QueueSize, PoolThroughput, ActiveTask>("Status während der Last");
  }

  SC::ChaosThreading::wait_until_finished();
  PoolLog::stats<PoolThroughput>("Test beendet. Finaler Durchsatz");

  SC::ChaosGlobal::report_all();
  return 0;
}
