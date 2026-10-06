//
// Created by oleub on 25.03.26.
//

#include "registry.hpp"

#include <thread>

namespace sc::ecs {
Entity Registry::create() {
  while (true) {
    // Acquire pairs with the release store that publishes a refilled buffer.
    const uint32_t index = m_entityNext.fetch_add(1, std::memory_order_acquire);
    if (index < ENTITY_BLOCK_SIZE) [[likely]] {
      const Entity entity = m_entities[index];
      m_entityRead.fetch_add(1, std::memory_order_release);
      return entity;
    }
    refillEntities();
  }
}

void Registry::refillEntities() {
  std::lock_guard lock{m_entityMutex};
  if (m_entityNext.load(std::memory_order_relaxed) < ENTITY_BLOCK_SIZE) {
    // Another thread refilled the buffer in the meantime.
    return;
  }

  // Every index of this round is taken, but some threads may not have read
  // their entity yet. They are a few instructions away from it.
  while (m_entityRead.load(std::memory_order_acquire) != ENTITY_BLOCK_SIZE) {
    std::this_thread::yield();
  }

  m_reg.create(internal::EntityOutputIterator{m_entities.begin(), this},
               internal::EntityOutputIterator{m_entities.end(), this});
  m_entityRead.store(0, std::memory_order_relaxed);
  m_entityNext.store(0, std::memory_order_release);
  Entities::record(ENTITY_BLOCK_SIZE);
}

} // namespace sc::ecs
