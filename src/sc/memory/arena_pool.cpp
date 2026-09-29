//
// Created by oleub on 05.03.26.
//

#include "arena_pool.hpp"

#include "arena.hpp"
#include "logger/logger.hpp"
#include "monotonic_resource.hpp"

namespace sc {
using PoolLogger = sc::Logger<"Mem">;
void ArenaPool::init(const size_t poolSize, const size_t arenaSize) {
  MonotonicResource res;
  if (!res.checkLockFree()) {
    PoolLogger::warn("ChaosMemoryResource BlockRange is not LockFree");
  }

  m_defaultSize = arenaSize;
  m_storage.reserve(MAX_LOADED_ARENAS);
  for (size_t i = 0; i < poolSize; ++i) {
    m_storage.push_back(std::make_unique<Arena>(m_defaultSize));
  }
}

std::unique_ptr<Arena> ArenaPool::acquire() {
  ArenaPool::m_lock.lock();
  if (m_storage.empty()) {
    ArenaPool::m_lock.unlock();

    return std::make_unique<Arena>(m_defaultSize);
  }

  auto arena = std::move(m_storage.back());
  m_storage.pop_back();
  ArenaPool::m_lock.unlock();
  return arena;
}

void ArenaPool::release(std::unique_ptr<Arena> &arena) {
  if (!arena)
    return;
  arena->reset();

  m_lock.lock();
  if (m_storage.size() >= MAX_LOADED_ARENAS) {
    arena.reset();
    m_lock.unlock();
    return;
  }

  m_storage.push_back(std::move(arena));
  m_lock.unlock();
}

size_t ArenaPool::getBufferSize() { return m_defaultSize; }
} // namespace sc
