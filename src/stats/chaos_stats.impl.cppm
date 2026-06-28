module;
#include <string>
#include <string_view>
#include <utility>

export module SC.Stats:Impl;
export import :Core;
import SC.Util;

namespace SC {

export template <FixedString Name, typename ChaosTracker,
                 bool AllowReset = true>
class ChaosStat : public IChaosStat {
public:
  // Nutzen wir nur, wenn Stats auch an sind
  static inline typename ChaosTracker::Storage m_storage{Name.text()};

  static void record(auto &&...args) {
    if constexpr (ChaosStatsEnabled) {
      ChaosTracker::record(m_storage, std::forward<decltype(args)>(args)...);
    }
  }

  static void start(auto &&...args) {
    if constexpr (ChaosStatsEnabled) {
      ChaosTracker::start(m_storage, std::forward<decltype(args)>(args)...);
    }
  }

  static void stop(auto &&...args) {
    if constexpr (ChaosStatsEnabled) {
      ChaosTracker::stop(m_storage, std::forward<decltype(args)>(args)...);
    }
  }

  static void reset() {
    if constexpr (ChaosStatsEnabled && AllowReset) {
      ChaosTracker::reset(m_storage);
    }
  }

  static constexpr std::string_view name() { return Name.text(); }

  static std::string str() {
    if constexpr (ChaosStatsEnabled)
      return ChaosTracker::format(m_storage);
    return "";
  }

private:
  // 1. Instanz-Getter zuerst definieren, damit er für m_registrar sichtbar ist
  static ChaosStat *get_instance() {
    static ChaosStat instance;
    return &instance;
  }

  // 2. Registrierungs-Struktur
  struct AutoReg {
    AutoReg(IChaosStat *ptr) { ChaosStats::register_stat(ptr); }
  };

  static inline AutoReg m_registrar{get_instance()};

  void internal_reset() override { reset(); }
  std::string internal_str() override { return str(); }
  std::string_view internal_name() override { return name(); }
};

export template <typename StatsType> struct ChaosScopeGuard {
  ChaosScopeGuard(auto &&...args) {
    StatsType::start(std::forward<decltype(args)>(args)...);
  }
  ~ChaosScopeGuard() { StatsType::stop(); }
};

} // namespace SC
