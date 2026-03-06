//
// Created by oleub on 04.03.26.
//

#pragma once
#include <atomic>
#include <chrono>

#include "chaos_units.hpp"

namespace SC {
  template<typename UnitSystem = DataUnits>
  struct ChaosThroughput {
    using DurationNS = std::chrono::nanoseconds;
    using TimePoint = std::chrono::high_resolution_clock::time_point;

    struct Storage {
      std::atomic<uint64_t> value{0};
      std::atomic<DurationNS::rep> accumulated_ns{0};
      std::atomic<TimePoint::duration::rep> startTimeTicks{0};
      std::atomic<bool> running{false};
    };

    static void start(Storage &s) {
      if (!s.running.exchange(true, std::memory_order_acquire)) {
        auto now = std::chrono::high_resolution_clock::now();
        s.startTimeTicks.store(now.time_since_epoch().count(), std::memory_order_release);
      }
    }

    static void record(Storage &s, uint64_t b) {
      start(s);
      s.value.fetch_add(b, std::memory_order_relaxed);
    }

    static void stop(Storage &s) {
      if (s.running.exchange(false, std::memory_order_acq_rel)) {
        auto end = std::chrono::high_resolution_clock::now();
        auto startTicks = s.startTimeTicks.load(std::memory_order_acquire);
        TimePoint start{TimePoint::duration{startTicks}};

        auto diff = std::chrono::duration_cast<DurationNS>(end - start);
        s.accumulated_ns.fetch_add(diff.count(), std::memory_order_relaxed);
      }
    }

    static void reset(Storage &s) {
      s.value.store(0, std::memory_order_relaxed);
      s.accumulated_ns.store(0, std::memory_order_relaxed);
      s.running.store(false, std::memory_order_relaxed);
    }

    static std::string format(const Storage &s) {
      uint64_t raw_val = s.value.load(std::memory_order_relaxed);
      DurationNS dur{s.accumulated_ns.load(std::memory_order_relaxed)};

      if (s.running.load(std::memory_order_acquire)) {
        auto start = std::chrono::high_resolution_clock::time_point{
          std::chrono::high_resolution_clock::duration{s.startTimeTicks.load(std::memory_order_acquire)}
        };
        dur += (std::chrono::high_resolution_clock::now() - start);
      }

      double secs = std::chrono::duration<double>(dur).count();
      double val_per_sec = (secs > 0) ? (static_cast<double>(raw_val) / secs) : 0.0;

      return std::format("{} total | {}/s",
                         ChaosFormatter<UnitSystem>::format(static_cast<double>(raw_val)),
                         ChaosFormatter<UnitSystem>::format(val_per_sec));
    }
  };
}


