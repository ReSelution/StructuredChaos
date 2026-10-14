#pragma once

#include <atomic>

#include "sc/threading/cpu_relax.hpp"

namespace sc {

  // For very short critical sections. Lockable, so it works with
  // std::scoped_lock and std::unique_lock.
  class alignas(64) SpinLock {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;

  public:
    SpinLock() noexcept = default;

    SpinLock(const SpinLock &) = delete;
    SpinLock &operator=(const SpinLock &) = delete;

    void lock() noexcept {
      while (flag.test_and_set(std::memory_order_acquire)) {
        while (flag.test(std::memory_order_relaxed)) {
          cpuRelax();
        }
      }
    }

    [[nodiscard]] bool try_lock() noexcept { return !flag.test_and_set(std::memory_order_acquire); }

    void unlock() noexcept { flag.clear(std::memory_order_release); }
  };

} // namespace sc
