//
// Created by oleub on 25.03.26.
//

#include "chaos_registry.hpp"

#include "chaos_entity.hpp"
#include "memory/chaos_arena_pool.hpp"

namespace SC {
  ChaosEntity ChaosRegistry::create() {
    size_t myIdx = mEntityIdx.fetch_add(1, std::memory_order_relaxed);

    if (myIdx >= ENTITY_BLOCK_SIZE) {
      createEntities();
      return create();
    }

    return {mEntities[myIdx], this};
  }

  void ChaosRegistry::createEntities() {
    std::lock_guard guard{m_regMutex};
    auto idx = mEntityIdx.load(std::memory_order_relaxed);
    if (idx < ENTITY_BLOCK_SIZE) {
      return;
    }
    m_reg.create(mEntities.begin(), mEntities.end());
    mEntityIdx.store(0, std::memory_order_relaxed);
  }
} // SC
