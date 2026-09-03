module;
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>

export module sc.stats:core;

namespace sc::stats {

  export constexpr bool StatsEnabled =
#ifdef CHAOS_STATS_ENABLED
      true;
#else
      false;
#endif

  export class IStat {
  public:
    virtual ~IStat()              = default;
    virtual void internal_reset() = 0;

    [[nodiscard]] virtual std::string internal_str() const       = 0;
    [[nodiscard]] virtual std::string_view internal_name() const = 0;
  };

  // Interne Registry-Zugriffsfunktion (bleibt im Namespace, aber nicht exportiert)
  std::unordered_map<std::string_view, IStat *> &reg() {
    static std::unordered_map<std::string_view, IStat *> registry{};
    return registry;
  }
  export void reset_all() {
    if constexpr (StatsEnabled) {
      for (auto *s: reg() | std::views::values)
        s->internal_reset();
    }
  }

  export template<typename LoggerType>
  void report_all(typename LoggerType::LogLevel level = LoggerType::LogLevel::info) {
    if constexpr (StatsEnabled) {
      for (auto *s: reg() | std::views::values) {
        LoggerType::log(level, "{} -> {}", s->internal_name(), s->internal_str());
      }
    }
  }

  export void report_all_to(auto &&callback) {
    if constexpr (StatsEnabled) {
      for (auto *s: reg() | std::views::values) {
        callback(s->internal_name(), s->internal_str());
      }
    }
  }

  export void register_stat(IStat *stat) {
    if constexpr (StatsEnabled) {
      reg().emplace(stat->internal_name(), stat);
    }
  }

  export [[nodiscard]] std::optional<IStat *> get_stat(std::string_view name) {
    if constexpr (StatsEnabled) {
      auto &registry = reg();
      if (auto it = registry.find(name); it != registry.end()) {
        return it->second;
      }
    }
    return std::nullopt;
  }

} // namespace sc::stats
