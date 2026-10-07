#pragma once
#include <string>
#include <string_view>
#include <utility>

#include "core.hpp"
#include "sc/util/fixed_string.hpp"

namespace sc::stats {

  template <FixedString Name, typename ChaosTracker, bool AllowReset = true> class Stat : public IStat {
  public:
    // Only used when stats are enabled.
    static inline typename ChaosTracker::Storage m_storage{Name.text()};

    static void record(auto &&...args) {
      if constexpr (StatsEnabled) {
        ensureRegistered();
        ChaosTracker::record(m_storage, std::forward<decltype(args)>(args)...);
      }
    }

    static void start(auto &&...args) {
      if constexpr (StatsEnabled) {
        ensureRegistered();
        ChaosTracker::start(m_storage, std::forward<decltype(args)>(args)...);
      }
    }

    static void stop(auto &&...args) {
      if constexpr (StatsEnabled) {
        ensureRegistered();
        ChaosTracker::stop(m_storage, std::forward<decltype(args)>(args)...);
      }
    }

    static void reset() {
      if constexpr (StatsEnabled && AllowReset) {
        ensureRegistered();
        ChaosTracker::reset(m_storage);
      }
    }

    static constexpr std::string_view name() { return Name.text(); }

    static std::string str() {
      if constexpr (StatsEnabled) {
        ensureRegistered();
        return ChaosTracker::format(m_storage);
      }
      return "";
    }

  private:
    // Defined before m_registrar, which needs it.
    static Stat *get_instance() {
      static Stat instance;
      return &instance;
    }

    // Adds the stat to the list of all stats when it is constructed.
    struct AutoReg {
      AutoReg(IStat *ptr) { stats::register_stat(ptr); }
    };

    static inline AutoReg m_registrar{get_instance()};

    // A static member of a class template only exists once something uses it.
    // Nothing else refers to m_registrar, so without this no stat would ever
    // register itself.
    static void ensureRegistered() noexcept { (void)&m_registrar; }

  public:
    void internal_reset() override { reset(); }
    [[nodiscard]] std::string internal_str() const override { return str(); }
    [[nodiscard]] std::string_view internal_name() const override { return name(); }
  };

  template <typename StatsType> struct ScopeGuard {
    ScopeGuard(auto &&...args) { StatsType::start(std::forward<decltype(args)>(args)...); }
    ~ScopeGuard() { StatsType::stop(); }

    // A copy would stop the stat a second time.
    ScopeGuard(const ScopeGuard &) = delete;
    ScopeGuard &operator=(const ScopeGuard &) = delete;
  };

} // namespace sc::stats
