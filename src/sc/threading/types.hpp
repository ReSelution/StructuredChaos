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

  // The threadId of every thread that is neither a pool worker nor the thread
  // that called init.
  inline constexpr uint32_t NO_THREAD_ID = UINT32_MAX;
} // namespace sc::threading
