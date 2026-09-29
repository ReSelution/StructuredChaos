#pragma once

#include "sc/logger/logger.hpp"

#include "sc/stats/counter.hpp"
#include "sc/stats/stats.hpp"
#include "sc/stats/throughput.hpp"
#include <cstdint>

namespace sc::threading {
using ThreadLog = sc::Logger<"Threading">;
using QueueSize = sc::stats::Stat<"QueueDepth", sc::stats::Counter<>>;
using ActiveTask = sc::stats::Stat<"ActiveTask", sc::stats::Counter<>>;
using PoolThroughput =
    sc::stats::Stat<"PoolTasks", sc::stats::Throughput<sc::stats::MetricUnits>>;
using LongRunningThreads =
    sc::stats::Stat<"LongRunningThreads", sc::stats::Counter<>>;

enum class Priority : int32_t {
  High,
  Normal,
  Low,
  PriorityCount,
};
} // namespace sc::threading
