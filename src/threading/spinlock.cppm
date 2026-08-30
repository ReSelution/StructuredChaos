module;

#include <atomic>
#include <immintrin.h>

export module sc.threading:spin_lock;

namespace sc {

  export class alignas(64) ChaosSpinLock {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;

  public:
    ChaosSpinLock() noexcept = default;

    ChaosSpinLock(const ChaosSpinLock &)            = delete;
    ChaosSpinLock &operator=(const ChaosSpinLock &) = delete;

    void lock() noexcept {
      while (flag.test_and_set(std::memory_order_acquire)) {
        while (flag.test(std::memory_order_relaxed)) {
          _mm_pause();
        }
      }
    }

    [[nodiscard]] bool try_lock() noexcept { return !flag.test_and_set(std::memory_order_acquire); }

    void unlock() noexcept { flag.clear(std::memory_order_release); }
  };

  export class ChaosSpinLockGuard {
    ChaosSpinLock &m_lock;

  public:
    explicit ChaosSpinLockGuard(ChaosSpinLock &lock) noexcept : m_lock(lock) { m_lock.lock(); }

    ~ChaosSpinLockGuard() { m_lock.unlock(); }

    ChaosSpinLockGuard(const ChaosSpinLockGuard &)            = delete;
    ChaosSpinLockGuard &operator=(const ChaosSpinLockGuard &) = delete;
  };

} // namespace sc
