//
// Created by oleub on 25.03.26.
//

#include "registry.hpp"

namespace sc::ecs {
Entity Registry::create() {
  size_t myIdx = mEntityIdx.fetch_add(1, std::memory_order_relaxed);

  if (myIdx >= ENTITY_BLOCK_SIZE) {
    createEntities();
    return create();
  }
  return mEntities[myIdx];
}

void Registry::createEntities() {
  std::lock_guard lock{eCreationLock};
  auto idx = mEntityIdx.load(std::memory_order_relaxed);
  if (idx < ENTITY_BLOCK_SIZE) {
    return;
  }
  m_reg.create(internal::EntityOutputIterator{mEntities.begin(), this},
               internal::EntityOutputIterator{mEntities.end(), this});
  mEntityIdx.store(0, std::memory_order_relaxed);
  Entities::record(ENTITY_BLOCK_SIZE);
}

} // namespace sc::ecs
