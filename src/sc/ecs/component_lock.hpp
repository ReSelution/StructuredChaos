#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace sc::ecs::internal {

// Reader/writer lock in a single counter: a positive value is the number of
// readers, -1 means a writer holds the lock.
//
// Meant for the short critical sections of the registry. A thread that cannot
// get the lock gives up its time slice and tries again instead of sleeping,
// and a steady stream of readers can keep a writer waiting.
class ComponentLock {
public:
  ComponentLock() noexcept = default;

  ComponentLock(const ComponentLock &) = delete;
  ComponentLock &operator=(const ComponentLock &) = delete;

  void lock_shared() noexcept {
    int32_t seen = m_state.load(std::memory_order_relaxed);
    while (true) {
      if (seen >= 0) {
        // A failed attempt means another reader got in, and seen is current
        // again, so there is nothing to wait for.
        if (m_state.compare_exchange_weak(seen, seen + 1,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed)) {
          return;
        }
        continue;
      }
      std::this_thread::yield();
      seen = m_state.load(std::memory_order_relaxed);
    }
  }

  void unlock_shared() noexcept {
    m_state.fetch_sub(1, std::memory_order_release);
  }

  void lock() noexcept {
    while (true) {
      // Only try to take the lock when it looks free: a failed attempt would
      // still pull the counter away from the thread that holds the lock.
      int32_t expected = 0;
      if (m_state.load(std::memory_order_relaxed) == 0 &&
          m_state.compare_exchange_weak(expected, WRITER,
                                        std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
        return;
      }
      // Yielding lets the holder finish. Spinning here instead was measurably
      // slower when several threads write the same component.
      std::this_thread::yield();
    }
  }

  void unlock() noexcept { m_state.store(0, std::memory_order_release); }

private:
  static constexpr int32_t WRITER = -1;

  alignas(64) std::atomic<int32_t> m_state{0};
};

} // namespace sc::ecs::internal
