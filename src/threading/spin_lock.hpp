#pragma once


#include <atomic>
#include <immintrin.h>

namespace sc {

class alignas(64) SpinLock {
  std::atomic_flag flag = ATOMIC_FLAG_INIT;

public:
  SpinLock() noexcept = default;

  SpinLock(const SpinLock &) = delete;
  SpinLock &operator=(const SpinLock &) = delete;

  void lock() noexcept {
    while (flag.test_and_set(std::memory_order_acquire)) {
      while (flag.test(std::memory_order_relaxed)) {
        _mm_pause();
      }
    }
  }

  [[nodiscard]] bool try_lock() noexcept {
    return !flag.test_and_set(std::memory_order_acquire);
  }

  void unlock() noexcept { flag.clear(std::memory_order_release); }
};

class SpinLockGuard {
  SpinLock &m_lock;

public:
  explicit SpinLockGuard(SpinLock &lock) noexcept : m_lock(lock) {
    m_lock.lock();
  }

  ~SpinLockGuard() { m_lock.unlock(); }

  SpinLockGuard(const SpinLockGuard &) = delete;
  SpinLockGuard &operator=(const SpinLockGuard &) = delete;
};

} // namespace sc
