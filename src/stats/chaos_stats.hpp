#pragma once
#include <string>
#include <format>

#include "chaos_global.hpp"

namespace SC {
  template<typename NameTag, typename ChaosTracker>
  class ChaosStats : public IChaosStat {
    friend class ChaosGlobal;


  public:
    static inline typename ChaosTracker::Storage m_storage;

    IChaosStat * get() {
      return this;
    }
    template<typename... Args>
    static void record(Args &&... args) {
      ChaosTracker::record(m_storage, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void start(Args &&... args) {
      ChaosTracker::start(m_storage, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void stop(Args &&... args) {
      ChaosTracker::stop(m_storage, std::forward<Args>(args)...);
    }

    static void reset() {
      ChaosTracker::reset(m_storage);
    }

    static constexpr std::string_view name() { return NameTag::name(); }

    [[nodiscard]] static std::string str() {
      return ChaosTracker::format(m_storage);
    }
  private:
    void internal_reset() override {
      reset();
    }

    std::string internal_str() override {
      return str();
    }

    std::string_view internal_name() override {
      return name();
    }
  };


  template<typename StatsType>
  struct ChaosScopeGuard {
    template<typename... Args>
    ChaosScopeGuard(Args &&... args) {
      StatsType::start(std::forward<Args>(args)...);
    }

    ~ChaosScopeGuard() {
      StatsType::stop();
    }
  };

#define DEFINE_CHAOS_STAT(AliasName, TagString, TrackerType) \
struct _Tag_##AliasName { static constexpr std::string_view name() { return TagString; } }; \
using AliasName = SC::ChaosStats<_Tag_##AliasName, TrackerType>;\


} // namespace SC
