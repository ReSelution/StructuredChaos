#pragma once

#include <algorithm>
#include <iterator>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Defines CHAOS_STATS_ENABLED when the project is configured with
// -Denable_chaos_stats=true. Without it the switch below would always read
// "off".
#include "sc/config/config.h"

namespace sc::stats {

  constexpr bool StatsEnabled =
#ifdef CHAOS_STATS_ENABLED
      true;
#else
      false;
#endif

  class IStat {
  public:
    virtual ~IStat() = default;
    virtual void internalReset() = 0;

    [[nodiscard]] virtual std::string internalStr() const = 0;
    [[nodiscard]] virtual std::string_view internalName() const = 0;
  };

  namespace internal {
    // The list of all stats, by name. Stats register themselves on first use,
    // from whatever thread that happens on, hence the mutex.
    struct StatRegistry {
      std::mutex mutex;
      std::unordered_map<std::string_view, IStat *> stats;
    };

    inline StatRegistry &statRegistry() {
      static StatRegistry registry{};
      return registry;
    }

    // The stats at this moment. Callers work on the copy without the lock, so
    // that what they call may use or register stats itself. The stats live
    // until the end of the program, so the pointers stay valid.
    inline std::vector<IStat *> statSnapshot() {
      StatRegistry &registry = statRegistry();
      std::scoped_lock lock(registry.mutex);
      std::vector<IStat *> stats;
      stats.reserve(registry.stats.size());
      std::ranges::copy(registry.stats | std::views::values, std::back_inserter(stats));
      return stats;
    }
  } // namespace internal

  void resetAll();

  template<typename LoggerType>
  void reportAll(typename LoggerType::LogLevel level = LoggerType::LogLevel::info) {
    if constexpr (StatsEnabled) {
      for (const IStat *s: internal::statSnapshot()) {
        LoggerType::log(level, "{} -> {}", s->internalName(), s->internalStr());
      }
    }
  }

  void reportAllTo(auto &&callback) {
    if constexpr (StatsEnabled) {
      for (const IStat *s: internal::statSnapshot()) {
        callback(s->internalName(), s->internalStr());
      }
    }
  }

  // Two different stats with the same name are a programming error: only the
  // first one would be found by name.
  void registerStat(IStat *stat);

  // The stat of that name, or null if there is none (or stats are disabled).
  [[nodiscard]] IStat *findStat(std::string_view name);
} // namespace sc::stats
