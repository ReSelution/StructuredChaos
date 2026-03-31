//
// Created by oleub on 05.03.26.
//


#include "chaos_arena_pool.hpp"

#include "chaos_bump_arena.hpp"
#include "chaos_monotonic_resource.hpp"

namespace SC {
  LOG_ALIAS(poolLog, "Chaos", "Mem")

  void ChaosArenaPool::init(const size_t poolSize, const size_t arenaSize) {
    ChaosMonotonicResource res;
    if (!res.checkLockFree()) {
      poolLog::warn("ChaosMemoryResource BlockRange is not LockFree");
    }

    m_defaultSize = arenaSize;
    m_storage.reserve(MAX_LOADED_ARENAS);
    for (size_t i = 0; i < poolSize; ++i) {
      m_storage.push_back(std::make_unique<ChaosBumpArena>(m_defaultSize));
    }
  }

  std::unique_ptr<ChaosBumpArena> ChaosArenaPool::acquire() {
    ChaosSpinLockGuard guard(ChaosArenaPool::m_lock);

    if (m_storage.empty()) {
      return std::make_unique<ChaosBumpArena>(m_defaultSize);
    }

    auto arena = std::move(m_storage.back());
    m_storage.pop_back();
    return arena;
  }

  void ChaosArenaPool::release(std::unique_ptr<ChaosBumpArena> &arena) {
    if (!arena) return;
    arena->reset();

    if (m_storage.size() >= MAX_LOADED_ARENAS) {
      arena.reset();
    }

    m_lock.lock();

    m_storage.push_back(std::move(arena));
    m_lock.unlock();
  }

  size_t ChaosArenaPool::getBufferSize() {
    return m_defaultSize;
  }
} // SC
