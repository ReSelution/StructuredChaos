#pragma once
#include <string>
#include <string_view>
#include <utility>

#include "sc/util/fixed_string.hpp"

#include "core.hpp"

namespace sc::stats {

  template<FixedString Name, typename ChaosTracker, bool AllowReset = true>
  class Stat : public IStat {
    // Only used when stats are enabled.
    static inline typename ChaosTracker::Storage s_storage{Name.text()};

  public:
    static void record(auto &&...args) {
      if constexpr (StatsEnabled) {
        ensureRegistered();
        ChaosTracker::record(s_storage, std::forward<decltype(args)>(args)...);
      }
    }

    static void start(auto &&...args) {
      if constexpr (StatsEnabled) {
        ensureRegistered();
        ChaosTracker::start(s_storage, std::forward<decltype(args)>(args)...);
      }
    }

    static void stop(auto &&...args) {
      if constexpr (StatsEnabled) {
        ensureRegistered();
        ChaosTracker::stop(s_storage, std::forward<decltype(args)>(args)...);
      }
    }

    static void reset() {
      if constexpr (StatsEnabled && AllowReset) {
        ensureRegistered();
        ChaosTracker::reset(s_storage);
      }
    }

    static constexpr std::string_view name() { return Name.text(); }

    static std::string str() {
      if constexpr (StatsEnabled) {
        ensureRegistered();
        return ChaosTracker::format(s_storage);
      }
      return "";
    }

  private:
    // Defined before s_registrar, which needs it.
    static Stat *instance() {
      static Stat instance;
      return &instance;
    }

    // Adds the stat to the list of all stats when it is constructed.
    struct AutoReg {
      explicit AutoReg(IStat *ptr) { stats::registerStat(ptr); }
    };

    static inline AutoReg s_registrar{instance()};

    // A static member of a class template only exists once something uses it.
    // Nothing else refers to s_registrar, so without this no stat would ever
    // register itself.
    static void ensureRegistered() noexcept { (void) &s_registrar; }

  public:
    void internalReset() override { reset(); }
    [[nodiscard]] std::string internalStr() const override { return str(); }
    [[nodiscard]] std::string_view internalName() const override { return name(); }
  };

  // Starts the stat on construction and stops it at the end of the scope.
  template<typename StatsType>
  struct StatScope {
    explicit StatScope(auto &&...args) { StatsType::start(std::forward<decltype(args)>(args)...); }
    ~StatScope() { StatsType::stop(); }

    // A copy would stop the stat a second time.
    StatScope(const StatScope &) = delete;
    StatScope &operator=(const StatScope &) = delete;
  };

} // namespace sc::stats
