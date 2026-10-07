#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace sc::ecs::internal {

// Reader/writer lock in a single counter. The low bits count the readers, the
// two top bits say that a writer holds the lock or is waiting for it.
//
// Meant for the short critical sections of the registry. A thread that cannot
// get the lock gives up its time slice and tries again instead of sleeping.
//
// Writers go first: as soon as one waits, new readers stay out until it had
// its turn, so a steady stream of readers cannot keep a writer waiting
// forever. The price is that a thread must not take the lock shared a second
// time while it still holds it: with a writer waiting in between, the second
// attempt waits for the writer, and the writer for the first.
class ComponentLock {
public:
  ComponentLock() noexcept = default;

  ComponentLock(const ComponentLock &) = delete;
  ComponentLock &operator=(const ComponentLock &) = delete;

  void lock_shared() noexcept {
    uint32_t seen = m_state.load(std::memory_order_relaxed);
    while (true) {
      if ((seen & (WRITER | WAITING)) == 0) {
        // A failed attempt means another reader got in or a writer showed
        // up, and seen is current again, so there is nothing to wait for.
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
      uint32_t seen = m_state.load(std::memory_order_relaxed);
      if ((seen & ~WAITING) == 0) {
        // Taking the lock drops the waiting mark. Other writers that still
        // wait put it back on their next round.
        if (m_state.compare_exchange_weak(seen, WRITER,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed)) {
          return;
        }
        continue;
      }
      if ((seen & WAITING) == 0) {
        m_state.fetch_or(WAITING, std::memory_order_relaxed);
      }
      // Yielding lets the holder finish. Spinning here instead was measurably
      // slower when several threads write the same component.
      std::this_thread::yield();
    }
  }

  void unlock() noexcept {
    // No reader can be counted while a writer holds the lock, so the state
    // is just the two marks. A plain store is cheaper than clearing only the
    // writer mark, and a writer that still waits puts its mark back on its
    // next round.
    m_state.store(0, std::memory_order_release);
  }

private:
  static constexpr uint32_t WRITER = uint32_t{1} << 31;
  static constexpr uint32_t WAITING = uint32_t{1} << 30;

  alignas(64) std::atomic<uint32_t> m_state{0};
};

} // namespace sc::ecs::internal
