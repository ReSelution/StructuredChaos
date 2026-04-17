//
// Created by oleub on 25.03.26.
//

#include "chaos_registry.hpp"

#include "memory/chaos_arena_pool.hpp"

namespace SC {
  ChaosEntity ChaosRegistry::create() {
    size_t myIdx = mEntityIdx.fetch_add(1, std::memory_order_relaxed);

    if (myIdx >= ENTITY_BLOCK_SIZE) {
      createEntities();
      return create();
    }
    CHAOS_RECORD(Entities, 1);
    return mEntities[myIdx];
  }

  void ChaosRegistry::createEntities() {
    std::unique_lock guard{m_regMutex};
    auto idx = mEntityIdx.load(std::memory_order_relaxed);
    if (idx < ENTITY_BLOCK_SIZE) {
      return;
    }
    m_reg.create(mEntities.begin(), mEntities.end());
    for (auto &e : mEntities) {
      e.registry = this;
    }
    mEntityIdx.store(0, std::memory_order_relaxed);
  }


} // SC
