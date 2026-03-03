#include "logger/chaos_logger.hpp"
#include <thread>


using namespace std::chrono_literals;

// 1. Tags und Aliase definieren
LOG_ALIAS(MainLog, "CORE");
LOG_ALIAS(NetLog, "NETWORK", "HTTP");

int main() {
  // --- Basis Test ---
  MainLog::info("Programm gestartet...");
  NetLog::debug("Versuche Verbindung zu Port {}", 8080);

  // --- Timer Test 1: Info (Standard) ---
  {
    auto t = MainLog::time("{} - Task 'Initialisierung' beendet für User {}", "Admin");
    std::this_thread::sleep_for(50ms);
  }

  // --- Timer Test 2: Debug Level + Positions-Argumente ---
  {

    auto t = NetLog::time(spdlog::level::debug,
                         "Request ID {1} brauchte genau {0} bis zum Timeout", 42);
    std::this_thread::sleep_for(1200us); // Mikrosekunden-Test
  }

  // --- Timer Test 3: Verschiedene Einheiten (Auto-Erkennung) ---
  {
    auto t1 = MainLog::time("{} - Schneller Task", "Short");
    std::this_thread::sleep_for(100ns);
  }

  {
    auto t2 = MainLog::time("{} - Langer Task", "Long");
    std::this_thread::sleep_for(1.2s);
  }

  // --- Fehlerfall Test ---
  NetLog::err("Kritischer Verbindungsfehler!");

  MainLog::info("Test beendet. Schau in den Ordner 'logs/'!");
  return 0;
}