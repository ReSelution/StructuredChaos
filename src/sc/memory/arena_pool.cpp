#include "arena_pool.hpp"

#include <algorithm>
#include <mutex>

#include "sc/logger/logger.hpp"

namespace sc {
  using PoolLogger = sc::Logger<"Mem">;

  ArenaPool::ArenaPool(size_t poolSize, size_t arenaSize) : m_arenaSize(arenaSize) {
    if (!MonotonicResource::checkLockFree()) {
      PoolLogger::warn("MonotonicResource bump pointer is not lock-free");
    }

    m_arenas.reserve(MAX_KEPT_ARENAS);
    for (size_t i = 0; i < std::min(poolSize, MAX_KEPT_ARENAS); ++i) {
      m_arenas.push_back(std::make_unique<MonotonicResource>(m_arenaSize));
    }
  }

  std::unique_ptr<MonotonicResource> ArenaPool::acquire() {
    {
      std::scoped_lock lock(m_lock);
      if (!m_arenas.empty()) {
        auto arena = std::move(m_arenas.back());
        m_arenas.pop_back();
        return arena;
      }
    }

    return std::make_unique<MonotonicResource>(m_arenaSize);
  }

  void ArenaPool::release(std::unique_ptr<MonotonicResource> arena) {
    if (!arena) {
      return;
    }
    // MonotonicResource::reset(), not unique_ptr::reset(): frees what the
    // arena holds and keeps the arena.
    (*arena).reset();

    {
      std::scoped_lock lock(m_lock);
      if (m_arenas.size() < MAX_KEPT_ARENAS) {
        m_arenas.push_back(std::move(arena));
        return;
      }
    }

    // The pool is full. The arena is freed when it goes out of scope, outside
    // the lock.
  }
} // namespace sc
