//
// Created by oleub on 24.03.26.
//

#include "chaos_spin_lock.hpp"
#include <immintrin.h> // Für _mm_pause()


namespace SC {
  void ChaosSpinLock::lock() {
    while (flag.test_and_set(std::memory_order_acquire)) {
      while (flag.test(std::memory_order_relaxed)) {
        _mm_pause();
      }
    }
  }

  void ChaosSpinLock::unlock() {
    flag.clear(std::memory_order_release);
  }
} // SC