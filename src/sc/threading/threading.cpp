#include "threading.hpp"

#include <algorithm>
#include <cstdint>
#include <thread>

namespace sc::threading {
  uint32_t defaultWorkerCount() noexcept {
    // hardware_concurrency() may report 0 if it does not know.
    return std::max(std::thread::hardware_concurrency(), 2U) - 1;
  }

  void init(uint32_t threads, uint32_t backgroundThreads) {
    internal::initPool(threads);
    initBackground(backgroundThreads);
  }

  void shutdown() {
    // First: a task of the lane may still put work on the pool.
    shutdownBackground();
    internal::shutdownPool();
  }

  void waitUntilFinished() { internal::waitUntilFinished(); }

  bool helpThread() { return internal::helpParallel(); }

  bool helpThread(LoopTicket ticket) { return internal::joinParallel(ticket.value); }

  size_t threadSlots() noexcept { return internal::threadSlots(); }
} // namespace sc::threading
