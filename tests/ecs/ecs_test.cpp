
import sc.threading;
import sc.ecs;
import sc.logger;
import sc.stats;

using RegLog = sc::logger::logger<"Registery">;

using RegThroug = sc::stats : Stat<"Registery emplace", sc::stats::Throughput<sc::stats::MetricUnits>>;


constexpr size_t TOTAL_ENTITIES = 100000;
constexpr size_t BATCH_SIZE     = 5000; // Tasks pro Enqueue-Welle


void run_stressTest() {}

int main() {
  sc::threading::init();
  run_stressTest();
}
