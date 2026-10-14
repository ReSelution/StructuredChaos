#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "units.hpp"

namespace sc::stats {

  template<typename UnitSystem = MetricUnits>
  struct Counter {
    struct alignas(64) Storage {
      const std::string_view name;
      std::atomic<uint64_t> value{0};
    };

    static void start(Storage &) noexcept {}

    static void stop(Storage &) noexcept {}

    static void record(Storage &s, uint64_t v) noexcept { s.value.fetch_add(v, std::memory_order_relaxed); }

    static void reset(Storage &s) noexcept { s.value.store(0, std::memory_order_relaxed); }

    [[nodiscard]] static std::string format(const Storage &s) {
      const uint64_t value = s.value.load(std::memory_order_relaxed);
      return ChaosFormatter<UnitSystem>::format(static_cast<double>(value));
    }
  };
} // namespace sc::stats
