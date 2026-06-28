module;
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

export module SC.Stats:Core;

namespace SC {

export constexpr bool ChaosStatsEnabled =
#ifdef CHAOS_STATS_ENABLED
    true;
#else
    false;
#endif

export class IChaosStat {
public:
  virtual ~IChaosStat() = default;
  virtual void internal_reset() = 0;
  virtual std::string internal_str() = 0;
  virtual std::string_view internal_name() = 0;
};

export class ChaosStats {
public:
  static void reset_all() {
    if constexpr (ChaosStatsEnabled) {
      for (auto *s : reg() | std::views::values)
        s->internal_reset();
    }
  }

  template <typename LoggerType> static void report_all(auto level) {
    if constexpr (ChaosStatsEnabled) {
      for (auto *s : reg() | std::views::values) {
        LoggerType::log(level, "{} -> {}", s->internal_name(),
                        s->internal_str());
      }
    }
  }

  static void report_all_to(auto &&callback) {
    if constexpr (ChaosStatsEnabled) {
      for (auto *s : reg() | std::views::values) {
        callback(s->internal_name(), s->internal_str());
      }
    }
  }

  static void register_stat(IChaosStat *stat) {
    if constexpr (ChaosStatsEnabled) {
      reg().emplace(stat->internal_name(), stat);
    }
  }

  [[nodiscard]] static std::optional<IChaosStat *>
  get_stat(std::string_view name) {
    if constexpr (ChaosStatsEnabled) {
      auto &registry = reg();
      if (auto it = registry.find(name); it != registry.end()) {
        return it->second;
      }
    }
    return std::nullopt;
  }

  static const std::map<std::string_view, IChaosStat *> &get_registry() {
    return reg();
  }

private:
  static std::map<std::string_view, IChaosStat *> &reg() {
    static std::map<std::string_view, IChaosStat *> registry{};
    return registry;
  }
};

} // namespace SC
