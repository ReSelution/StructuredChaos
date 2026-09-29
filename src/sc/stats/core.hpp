#pragma once
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>

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

// Interne Registry-Zugriffsfunktion (bleibt im Namespace, aber nicht
// exportiert)
inline std::unordered_map<std::string_view, IStat *> &reg() {
  static std::unordered_map<std::string_view, IStat *> registry{};
  return registry;
}
void reset_all();

template <typename LoggerType>
void report_all(
    typename LoggerType::LogLevel level = LoggerType::LogLevel::info) {
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
