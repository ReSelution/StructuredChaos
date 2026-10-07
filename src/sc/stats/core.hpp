#pragma once
// Defines CHAOS_STATS_ENABLED when the project is configured with
// -Denable_chaos_stats=true. Has to come first: without it the switch below
// would always read "off".
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>

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
    virtual void internal_reset() = 0;

    [[nodiscard]] virtual std::string internal_str() const = 0;
    [[nodiscard]] virtual std::string_view internal_name() const = 0;
  };

  // The list of all stats, by name. Internal.
  inline std::unordered_map<std::string_view, IStat *> &reg() {
    static std::unordered_map<std::string_view, IStat *> registry{};
    return registry;
  }
  void reset_all();

  template <typename LoggerType> void report_all(typename LoggerType::LogLevel level = LoggerType::LogLevel::info) {
    if constexpr (StatsEnabled) {
      for (auto *s : reg() | std::views::values) {
        LoggerType::log(level, "{} -> {}", s->internal_name(), s->internal_str());
      }
    }
  }

  void report_all_to(auto &&callback) {
    if constexpr (StatsEnabled) {
      for (auto *s : reg() | std::views::values) {
        callback(s->internal_name(), s->internal_str());
      }
    }
  }

  void register_stat(IStat *stat);

  [[nodiscard]] std::optional<IStat *> get_stat(std::string_view name);
} // namespace sc::stats
