//
// Created by oleub on 05.03.26.
//

#include "arena_pool.hpp"

#include <mutex>

#include "logger/logger.hpp"
#include "monotonic_resource.hpp"

namespace sc {
  using PoolLogger = sc::Logger<"Mem">;
  void ArenaPool::init(const size_t poolSize, const size_t arenaSize) {
    std::scoped_lock lock(m_initLock);
    if (m_storage.capacity() > 0) [[unlikely]] {
      return;
    }
    if (!MonotonicResource::checkLockFree()) {
      PoolLogger::warn("MonotonicResource bump pointer is not lock-free");
    }

    m_defaultSize = arenaSize;
    m_storage.reserve(MAX_LOADED_ARENAS);
    for (size_t i = 0; i < poolSize; ++i) {
      m_storage.push_back(std::make_unique<MonotonicResource>(m_defaultSize));
    }
  }

  std::unique_ptr<MonotonicResource> ArenaPool::acquire() {
    {
      SpinLockGuard guard(m_lock);
      if (!m_storage.empty()) {
        auto resource = std::move(m_storage.back());
        m_storage.pop_back();
        return resource;
      }
    }

    return std::make_unique<MonotonicResource>(m_defaultSize);
  }

  void ArenaPool::release(std::unique_ptr<MonotonicResource> &resource) {
    if (!resource) {
      return;
    }
    (*resource).reset();

    {
      SpinLockGuard guard(m_lock);
      if (m_storage.size() < MAX_LOADED_ARENAS) {
        m_storage.push_back(std::move(resource));
        return;
      }
    }

    // The pool is full. Freed outside the lock.
    resource = nullptr;
  }

  size_t ArenaPool::getBufferSize() { return m_defaultSize; }
} // namespace sc
