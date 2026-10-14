#pragma once
#include <cstddef>
#include <memory>
#include <vector>

#include "sc/memory/monotonic_resource.hpp"
#include "sc/threading/spin_lock.hpp"

namespace sc {

  // Keeps arenas that were released for the next acquire, so that their
  // blocks need not be taken from the system again.
  //
  // acquire() and release() may be called from several threads at the same
  // time.
  class ArenaPool {
  public:
    static constexpr size_t DEFAULT_ARENA_SIZE = size_t{16} * 1024;
    // Arenas released beyond this are freed instead of kept.
    static constexpr size_t MAX_KEPT_ARENAS = 1024;

    // Starts with poolSize arenas of arenaSize bytes each.
    explicit ArenaPool(size_t poolSize = 0, size_t arenaSize = DEFAULT_ARENA_SIZE);

    ArenaPool(const ArenaPool &) = delete;
    ArenaPool &operator=(const ArenaPool &) = delete;

    // A kept arena, or a new one if there is none.
    [[nodiscard]] std::unique_ptr<MonotonicResource> acquire();

    // Resets the arena, which destroys what it holds, and keeps it for the
    // next acquire().
    void release(std::unique_ptr<MonotonicResource> arena);

    [[nodiscard]] size_t arenaSize() const noexcept { return m_arenaSize; }

  private:
    const size_t m_arenaSize;
    SpinLock m_lock;
    // Reserved for MAX_KEPT_ARENAS up front, so that nothing is allocated
    // while the spin lock is held.
    std::vector<std::unique_ptr<MonotonicResource>> m_arenas;
  };
} // namespace sc
