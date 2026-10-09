#include "registry.hpp"

#include <thread>
#include <vector>

namespace sc::ecs {
  Registry::Registry() {
    std::scoped_lock lock{liveMutex};
    live.push_back(this);
  }

  Registry::~Registry() {
    std::scoped_lock lock{liveMutex};
    std::erase(live, this);
  }

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
    std::scoped_lock lock{m_entityMutex};
    if (m_entityNext.load(std::memory_order_relaxed) < ENTITY_BLOCK_SIZE) {
      // Another thread refilled the buffer in the meantime.
      return;
    }

    // Every index of this round is taken, but some threads may not have read
    // their entity yet. They are a few instructions away from it.
    while (m_entityRead.load(std::memory_order_acquire) != ENTITY_BLOCK_SIZE) {
      std::this_thread::yield();
    }

    m_reg.create(internal::EntityOutputIterator{.current = m_entities.begin(), .reg = this},
                 internal::EntityOutputIterator{.current = m_entities.end(), .reg = this});
    m_entityRead.store(0, std::memory_order_relaxed);
    m_entityNext.store(0, std::memory_order_release);
    Entities::record(ENTITY_BLOCK_SIZE);
  }

  bool Registry::valid(entt::entity e) const {
    std::scoped_lock lock{m_entityMutex};
    return m_reg.valid(e);
  }

  bool Registry::destroy(entt::entity e) {
    if (!valid(e)) {
      return false;
    }

    // Not through m_reg.destroy: that walks the pool table, which is only
    // touched under the context lock.
    for (internal::ComponentAccessBase *access : m_allAccesses) {
      access->storageBase->remove(e);
    }

    std::scoped_lock lock{m_entityMutex};
    m_reg.storage<entt::entity>().erase(e);
    return true;
  }

} // namespace sc::ecs
