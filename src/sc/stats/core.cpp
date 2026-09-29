#include "core.hpp"
namespace sc::stats {
void reset_all() {
  if constexpr (StatsEnabled) {
    for (auto *s : reg() | std::views::values)
      s->internal_reset();
  }
}
void register_stat(IStat *stat) {
  if constexpr (StatsEnabled) {
    reg().emplace(stat->internal_name(), stat);
  }
}

[[nodiscard]] std::optional<IStat *> get_stat(std::string_view name) {
  if constexpr (StatsEnabled) {
    auto &registry = reg();
    if (auto it = registry.find(name); it != registry.end()) {
      return it->second;
    }
  }
  return std::nullopt;
}
} // namespace sc::stats
