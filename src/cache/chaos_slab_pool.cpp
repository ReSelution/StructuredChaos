//
// Created by oleub on 05.03.26.
//

#include "chaos_slab_pool.hpp"

namespace SC {

  void ChaosSlabPool::init(const size_t poolSize, const size_t arenaSize) {
    m_defaultSize = arenaSize;
    m_storage.reserve(poolSize);
    for (size_t i = 0; i < poolSize; ++i) {
      m_storage.push_back(std::make_unique<ChaosSlab>(m_defaultSize));
    }
  }

  std::unique_ptr<ChaosSlab> ChaosSlabPool::acquire() {
    while (m_lock.test_and_set(std::memory_order_acquire)) {
    }

    if (m_storage.empty()) {
      m_lock.clear(std::memory_order_release);
      return std::make_unique<ChaosSlab>(m_defaultSize);
    }

    auto arena = std::move(m_storage.back());
    m_storage.pop_back();
    m_lock.clear(std::memory_order_release);
    return arena;
  }

  void ChaosSlabPool::release(std::unique_ptr<ChaosSlab> &arena)  {
    if (!arena) return;
    arena->reset();

    while (m_lock.test_and_set(std::memory_order_acquire)) {
    }
    m_storage.push_back(std::move(arena));
    m_lock.clear(std::memory_order_release);
  }
} // SC