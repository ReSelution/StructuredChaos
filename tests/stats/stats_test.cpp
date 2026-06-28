#include <cassert>
#include <iostream>

import SC.Logger; // Dein normaler Logger
import SC.Stats;  // Dein Stats-System

using GlobalStatsLog = SC::ChaosLogger<"Stats">;
// Eine konkrete Test-Statistik, um das System zu füttern
class VulkanFrameTime : public SC::IChaosStat {
  std::string value_{"16.6ms"};

public:
  void internal_reset() override { value_ = "0ms"; }
  std::string internal_str() override { return value_; }
  std::string_view internal_name() override {
    return "engine.vulkan.frame_time";
  }

  // Statische Helfer für deinen neuen Template-Logger-Ansatz
  static std::string_view name() { return "engine.vulkan.frame_time"; }
  static std::string str() { return "16.6ms"; }
};

int main(int argc, char *argv[]) {
  std::cout << "[TEST] Starte SC.Stats Validierung mit spdlog...\n";

  if constexpr (!SC::ChaosStatsEnabled) {
    std::cout
        << "[WARNING] CHAOS_STATS_ENABLED ist deaktiviert. Test abgebrochen.\n";
    return 0;
  }

  // 1. Instanziieren und Registrieren für die Laufzeit-Registry
  VulkanFrameTime frame_stat;
  SC::ChaosStats::register_stat(&frame_stat);

  // 2. Test: Einzelne Statistik per Name aus dem System fischen (get_stat)
  auto found_stat = SC::ChaosStats::get_stat("engine.vulkan.frame_time");
  assert(found_stat.has_value() &&
         "Fehler: Stat wurde per Name nicht gefunden!");
  assert((*found_stat)->internal_str() == "16.6ms" &&
         "Fehler: Falscher Wert in der Stat!");

  // 3. Test: Fehlerfall bei get_stat
  auto missing_stat = SC::ChaosStats::get_stat("engine.vulkan.non_existent");
  assert(!missing_stat.has_value() &&
         "Fehler: System hat eine Geister-Statistik gefunden!");

  std::cout << "\n--- spdlog Ausgabe (report_all) ---\n";
  SC::ChaosStats::report_all<GlobalStatsLog>(SC::LogLevel::info);
  std::cout << "-----------------------------------\n";

  // 5. Test: Der compile-time Variadic-Template Aufruf deines Loggers
  // Das ist die Variante, die du gebaut hast, um Cycles komplett zu killen!
  std::cout << "\n--- spdlog Ausgabe (Direct Compile-Time Log) ---\n";
  GlobalStatsLog::stats<VulkanFrameTime>("Render-Pipeline läuft stabil");
  std::cout << "------------------------------------------------\n";

  // 6. Test: Reset-Logik prüfen
  SC::ChaosStats::reset_all();
  assert((*found_stat)->internal_str() == "0ms" &&
         "Fehler: Reset hat nicht gegriffen!");

  std::cout << "\n[SUCCESS] Alle Tests für SC.Stats und den Logger "
               "erfolgreich! 🏎️💨\n";
  return 0;
}
