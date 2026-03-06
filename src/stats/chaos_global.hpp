//
// Created by oleub on 04.03.26.
//

#pragma once
#include <map>
#include <ranges>
#include <string>
#include <vector>

#include "logger/chaos_logger.hpp"

namespace SC {
  class IChaosStat {
    friend class ChaosGlobal;
  public:
    virtual ~IChaosStat() = default;

  private:
    virtual  void internal_reset() = 0;

    virtual std::string internal_str() = 0;
    virtual std::string_view internal_name() = 0;

  };

  LOG_ALIAS(GlobalStatsLog, "Global Stats");

  class ChaosGlobal {
    public:
    static void reset_all() { for (const auto s: registry | std::views::values) s->internal_reset(); }

    static void report_all(spdlog::level::level_enum level = spdlog::level::info) {
      for (const auto s: registry | std::views::values) {
        GlobalStatsLog::info("{} -> {}", s->internal_name(), s->internal_str());
      }
    }
    static void register_stat(IChaosStat *stat) {
      registry.emplace(stat->internal_name(), stat);
    }
    static inline std::map<std::string_view, IChaosStat *> registry{};
  };

#define REGISTER_CHAOS_STAT(AliasName) \
static inline bool _reg_##AliasName = []() { \
static AliasName _instance_##AliasName{}; \
SC::ChaosGlobal::register_stat(&_instance_##AliasName); \
return true; \
}();
} // SC
