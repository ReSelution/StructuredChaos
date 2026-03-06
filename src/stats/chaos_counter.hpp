//
// Created by oleub on 04.03.26.
//

#pragma once
#include <atomic>
#include <format>
#include <string>

#include "chaos_units.hpp"

namespace SC {
  struct MetricUnits;

  template<typename UnitSystem = MetricUnits>
  struct ChaosCounter {
    struct Storage {
      std::atomic<uint64_t> value{0};
    };

    // Für das Interface-Matching (macht hier einfach nichts)
    static void start(Storage &) {
    }

    static void stop(Storage &) {
    }

    static void record(Storage &s, uint64_t v) {
      s.value.fetch_add(v, std::memory_order_relaxed);
    }

    static void reset(Storage &s) {
      s.value.store(0, std::memory_order_relaxed);
    }

    static std::string format(const Storage &s) {
      uint64_t raw_val = s.value.load(std::memory_order_relaxed);
      return std::format("{}", ChaosFormatter<UnitSystem>::format(static_cast<double>(raw_val)));
    }
  };
}
