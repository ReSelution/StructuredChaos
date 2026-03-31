//
// Created by oleub on 25.03.26.
//

#pragma once
#include <mutex>
#include <entt/entt.hpp>

#include "threading/chaos_spin_lock.hpp"
#include <shared_mutex>

template<typename T>
struct get_class_from_member;

template<typename T, typename C>
struct get_class_from_member<T C::*> {
  using type = C;
};

template<auto Method>
concept IsRegistryMethod = requires
{
  requires std::is_member_function_pointer_v<decltype(Method)>;
} && std::is_same_v<typename get_class_from_member<decltype(Method)>::type, entt::registry>;

namespace SC {
  class ChaosEntity;

  class ChaosRegistry {
    template<typename Component>
    struct alignas(64) ComponentAccess {
      std::shared_mutex mutex;
      bool created = false;
    };

    static constexpr uint32_t ENTITY_BLOCK_SIZE = 512;
    using entity = entt::entity;

  public:
    ChaosEntity create();

    template<typename Component, typename... Args>
    decltype(auto) emplace(entity e, Args &&... args) {
      return execute<Component, &entt::registry::emplace>(e, args...);
    }


    template<typename Component>
    decltype(auto) get(entt::entity e) const;

    template<typename Component, auto Method, typename... Args>
      requires IsRegistryMethod<Method>
    decltype(auto) execute(entity e, Args &&... args);

  private:
    void createEntities();

    template<typename T>
    auto *getComponentAccess() {
      auto *acc = m_reg.ctx().find<T>();

      if (!acc) {
        std::lock_guard lock(m_regMutex);
        acc = m_reg.ctx().find<T>();
        if (!acc) {
          acc = &m_reg.ctx().emplace<T>();
        }
      }
      return acc;
    }


    alignas(64) std::mutex m_regMutex;
    entt::registry m_reg;

    std::array<entity, ENTITY_BLOCK_SIZE> mEntities{};
    alignas(64)std::atomic<uint32_t> mEntityIdx{ENTITY_BLOCK_SIZE};
  };

  template<typename Component>
  decltype(auto) ChaosRegistry::get(entt::entity e) const {
    auto &acc = getComponentAccess<Component>();
    std::shared_lock lock(acc.mutex);
    return m_reg.get<Component>(e);
  }

  template<typename Component, auto Method, typename... Args> requires IsRegistryMethod<Method>
  decltype(auto) ChaosRegistry::execute(entity e, Args &&... args) {
    using AccessType = ComponentAccess<Component>;
    auto *acc = getComponentAccess<AccessType>();

    auto doCall = [&]() -> decltype(auto) {
      return (m_reg.*Method).template operator()<Component>(e, std::forward<Args>(args)...);
    };
    std::unique_lock lock(acc->mutex);

    if (!acc->created) {
      std::lock_guard guard(m_regMutex);
      if (!acc->created) {
        decltype(auto) ret = doCall();
        acc->created = true;
        return ret;
      }
    }

    return doCall();
  }
} // SC
