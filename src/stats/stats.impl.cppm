module;
#include <string>
#include <string_view>
#include <utility>

export module sc.stats:impl;
export import :core;
import sc.util;

namespace sc::stats {

  export template<FixedString Name, typename ChaosTracker, bool AllowReset = true>
  class Stat : public IStat {
  public:
    // Nutzen wir nur, wenn Stats auch an sind
    static inline typename ChaosTracker::Storage m_storage{Name.text()};

    static void record(auto &&...args) {
      if constexpr (StatsEnabled) {
        ChaosTracker::record(m_storage, std::forward<decltype(args)>(args)...);
      }
    }

    static void start(auto &&...args) {
      if constexpr (StatsEnabled) {
        ChaosTracker::start(m_storage, std::forward<decltype(args)>(args)...);
      }
    }

    static void stop(auto &&...args) {
      if constexpr (StatsEnabled) {
        ChaosTracker::stop(m_storage, std::forward<decltype(args)>(args)...);
      }
    }

    static void reset() {
      if constexpr (StatsEnabled && AllowReset) {
        ChaosTracker::reset(m_storage);
      }
    }

    static constexpr std::string_view name() { return Name.text(); }

    static std::string str() {
      if constexpr (StatsEnabled)
        return ChaosTracker::format(m_storage);
      return "";
    }

  private:
    // 1. Instanz-Getter zuerst definieren, damit er für m_registrar sichtbar ist
    static Stat *get_instance() {
      static Stat instance;
      return &instance;
    }

    // 2. Registrierungs-Struktur
    struct AutoReg {
      AutoReg(IStat *ptr) { stats::register_stat(ptr); }
    };

    static inline AutoReg m_registrar{get_instance()};

    void internal_reset() override { reset(); }
    std::string internal_str() const override { return str(); }
    std::string_view internal_name() const override { return name(); }
  };

  export template<typename StatsType>
  struct ScopeGuard {
    ScopeGuard(auto &&...args) { StatsType::start(std::forward<decltype(args)>(args)...); }
    ~ScopeGuard() { StatsType::stop(); }
  };

} // namespace sc::stats
