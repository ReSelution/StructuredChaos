#pragma once

#include <cstddef>
#include <cstdint>

#include "sc/logger/logger.hpp"
#include "sc/stats/counter.hpp"
#include "sc/stats/stats.hpp"

namespace sc::threading {
  using ThreadLog = sc::Logger<"Threading">;
  using QueueSize = sc::stats::Stat<"QueueDepth", sc::stats::Counter<>>;
  using ActiveTask = sc::stats::Stat<"ActiveTask", sc::stats::Counter<>>;

  // The ticket of a running parallelFor loop, see parallelFor with onTicket
  // and helpThread(ticket). Once the loop is over it is void and helpThread
  // does nothing with it, as with a default constructed one.
  struct LoopTicket {
    uint64_t value = 0;
  };

  enum class Priority : int32_t {
    High,
    Normal,
    Low,
  };

  inline constexpr size_t PRIORITY_COUNT = 3;
} // namespace sc::threading
