#include "core.hpp"

#include <cassert>

namespace sc::stats {
  void resetAll() {
    if constexpr (StatsEnabled) {
      internal::StatRegistry &registry = internal::statRegistry();
      std::scoped_lock lock(registry.mutex);
      for (IStat *s: registry.stats | std::views::values) {
        s->internalReset();
      }
    }
  }

  void registerStat(IStat *stat) {
    if constexpr (StatsEnabled) {
      internal::StatRegistry &registry = internal::statRegistry();
      std::scoped_lock lock(registry.mutex);
      [[maybe_unused]] const auto [it, inserted] = registry.stats.emplace(stat->internalName(), stat);
      assert((inserted || it->second == stat) && "sc::stats: two stats with the same name");
    }
  }

  IStat *findStat(std::string_view name) {
    if constexpr (StatsEnabled) {
      internal::StatRegistry &registry = internal::statRegistry();
      std::scoped_lock lock(registry.mutex);
      if (auto it = registry.stats.find(name); it != registry.stats.end()) {
        return it->second;
      }
    }
    return nullptr;
  }
} // namespace sc::stats
