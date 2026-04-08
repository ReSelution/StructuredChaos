//
// Created by oleub on 25.03.26.
//

#pragma once
#include <memory_resource>
#include <mutex>
#include <entt/entt.hpp>

#include "threading/chaos_spin_lock.hpp"
#include <shared_mutex>

namespace SC {
  class ChaosEntity;

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


  template<typename T>
  concept IsChaosEntity = std::is_same_v<T, SC::ChaosEntity>;

  struct PoolAnchor {
    static inline thread_local std::pmr::memory_resource *current = nullptr;
  };


  class ChaosRegistry {
    template<typename Component>
    struct alignas(64) ComponentAccess {
      std::shared_mutex mutex;
      std::pmr::synchronized_pool_resource pool;
      bool created = false;
    };

    static constexpr uint32_t ENTITY_BLOCK_SIZE = 512;
    using entity = entt::entity;

    std::vector<std::pmr::synchronized_pool_resource *> m_pools{};

  public:
    ~ChaosRegistry() {
      m_reg.clear();
      for (auto pool: m_pools) {
        pool->release();
      }
    }

    ChaosEntity create();

    template<typename It>
      requires IsChaosEntity<typename std::iterator_traits<It>::value_type>
    void create(It begin, It end);

    template<typename Component, typename... Args>
    decltype(auto) emplace(const entity e, Args &&... args) {
      return executeWrite<Component>([&]() -> decltype(auto) {
        return m_reg.emplace<Component>(e, std::forward<Args>(args)...);
      });
    }

    template<typename Component>
    void setStorageAnchor() {
      auto *acc = getComponentAccess<Component>();
      PoolAnchor::current = &acc->pool;
    }


    template<typename Component, typename It, typename DataIt>
    void insert(It first, It last, DataIt dataBegin) {
      executeWrite<Component>([&]() -> void {
         m_reg.insert<Component>(first, last, dataBegin);
      });
    }

    template<typename Component>
    decltype(auto) get(entt::entity e) const;

    template<typename Component, typename Func>
    decltype(auto) executeWrite(Func &&func);

  private:
    void createEntities();

    template<typename Component>
    auto *getComponentAccess();

    alignas(64) std::shared_mutex m_regMutex;
    entt::registry m_reg;

    std::array<entity, ENTITY_BLOCK_SIZE> mEntities{};
    alignas(64)std::atomic<uint32_t> mEntityIdx{ENTITY_BLOCK_SIZE};
  };

  class ChaosEntity {
    friend class ChaosRegistry;
    template<typename, typename, typename, typename>
    friend class entt::basic_storage;

  public:
    ChaosEntity(entt::entity entity, ChaosRegistry *registry);


    ChaosEntity() = default;

    ChaosEntity(const ChaosEntity &) = default;

    ChaosEntity &operator=(const ChaosEntity &) = default;

    operator bool() const {
      return entity != entt::null && registry != nullptr;
    }

    operator entt::entity() const {
      return entity;
    }


    template<typename T, typename... Args>
    decltype(auto) add(Args &&... args);

    template<typename T>
    decltype(auto) get() const {
      return registry->get<T>(entity);
    }

  private:
    ChaosEntity(entt::entity entity);

    entt::entity entity{entt::null};
    ChaosRegistry *registry{nullptr};
  };


  template<typename It> requires IsChaosEntity<typename std::iterator_traits<It>::value_type>
  void ChaosRegistry::create(It begin, It end) {
    std::unique_lock guard{m_regMutex};
    m_reg.create(begin, end);
    for (auto iter = begin; iter != end; ++iter) {
      iter->registry = this;
    }
  }

  template<typename Component>
  decltype(auto) ChaosRegistry::get(entt::entity e) const {
    auto &acc = getComponentAccess<Component>();
    std::shared_lock lock(acc.mutex);
    return m_reg.get<Component>(e);
  }

  template<typename Component, typename Func>
  decltype(auto) ChaosRegistry::executeWrite(Func &&func) {
    auto *acc = getComponentAccess<Component>();
    static_assert(std::is_move_constructible_v<Component>, "Component must be moveable");
    static_assert(std::is_nothrow_move_constructible_v<Component>, "Move must be no-throw for EnTT performance");
    static_assert(std::is_trivially_destructible_v<Component>);

    PoolAnchor::current = &acc->pool;
    std::unique_lock lock(acc->mutex);
    if (!acc->created) [[unlikely]]{
      std::unique_lock regLock(m_regMutex);
      if (!acc->created) {
        if constexpr (std::is_void_v<std::invoke_result_t<Func>>) {
          func();
          acc->created = true;
          return;
        }else {
          decltype(auto) ret = func();
          acc->created = true;
          return ret;
        }

      }
    }

    return func();
  }

  template<typename Component>
  auto *ChaosRegistry::getComponentAccess() {
    using AccessType = ComponentAccess<Component>;
    {
      std::shared_lock lock(m_regMutex);
      if (auto *uptr = m_reg.ctx().find<std::unique_ptr<AccessType> >()) {
        return uptr->get();
      }
    }

    std::unique_lock lock(m_regMutex);
    if (auto *uptr = m_reg.ctx().find<std::unique_ptr<AccessType> >()) {
      return uptr->get();
    }

    auto uptr = std::make_unique<AccessType>();
    auto *ptr = uptr.get();
    m_reg.ctx().emplace<std::unique_ptr<AccessType> >(std::move(uptr));
    m_pools.emplace_back(&ptr->pool);
    return ptr;
  }


  template<typename T, typename... Args>
  decltype(auto) ChaosEntity::add(Args &&... args) {
    return registry->emplace<T>(entity, std::forward<Args>(args)...);
  }
} // SC
