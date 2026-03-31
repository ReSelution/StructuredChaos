//
// Created by oleub on 24.03.26.
//

#pragma once
#include <atomic>

namespace SC {
  class alignas(64) ChaosSpinLock {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;

  public:
    void lock();

    void unlock();
  };


  class ChaosSpinLockGuard {
    ChaosSpinLock &m_lock;

  public:
    explicit ChaosSpinLockGuard(ChaosSpinLock &lock) noexcept : m_lock(lock) {
      m_lock.lock();
    }

    ~ChaosSpinLockGuard() {
      m_lock.unlock();
    }
  };
} // SC
