//
// Created by oleub on 05.03.26.
//

#include "arena_pool.hpp"

#include "arena.hpp"
#include "logger/logger.hpp"
#include "monotonic_resource.hpp"
#include <mutex>

namespace sc {
using PoolLogger = sc::Logger<"Mem">;
void ArenaPool::init(const size_t poolSize, const size_t arenaSize) {
  std::lock_guard<std::mutex> lock(m_initLock);
  if (m_storage.capacity() > 0) [[unlikely]] {
    return;
  }
  if (!MonotonicResource::checkLockFree()) {
    PoolLogger::warn("MonotonicResource BlockRange is not lock-free");
  }

  m_defaultSize = arenaSize;
  m_storage.reserve(MAX_LOADED_ARENAS);
  for (size_t i = 0; i < poolSize; ++i) {
    m_storage.push_back(std::make_unique<Arena>(m_defaultSize));
  }
}

std::unique_ptr<Arena> ArenaPool::acquire() {
  {
    SpinLockGuard guard(m_lock);
    if (!m_storage.empty()) {
      auto arena = std::move(m_storage.back());
      m_storage.pop_back();
      return arena;
    }
  }

  return std::make_unique<Arena>(m_defaultSize);
}

void ArenaPool::release(std::unique_ptr<Arena> &arena) {
  if (!arena)
    return;
  arena->reset();

  {
    SpinLockGuard guard(m_lock);
    if (m_storage.size() < MAX_LOADED_ARENAS) {
      m_storage.push_back(std::move(arena));
      return;
    }
  }

  // The pool is full. Freed outside the lock.
  arena.reset();
}

size_t ArenaPool::getBufferSize() { return m_defaultSize; }
} // namespace sc
