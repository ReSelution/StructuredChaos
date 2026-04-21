//
// Created by oleub on 25.03.26.
//

#pragma once

#include <memory_resource>
#include <mutex>
#include <entt/entt.hpp>

#include <shared_mutex>
#include <atomic>

#include "anchor.hpp"
#include "chaos_resource.hpp"

#include "pfr.hpp"

#include "stats/chaos_stats.hpp"
#include "stats/chaos_throughput.hpp"

namespace SC {
  // template<typename T>
  // struct get_class_from_member;
  //
  // template<typename T, typename C>
  // struct get_class_from_member<T C::*> {
  //   using type = C;
  // };
  //
  // template<auto Method>
  // concept IsRegistryMethod = requires
  // {
  //   requires std::is_member_function_pointer_v<decltype(Method)>;
  // } && std::is_same_v<typename get_class_from_member<decltype(Method)>::type, entt::registry>;

  class ChaosRegistry;

  class ChaosEntity {
    friend class ChaosRegistry;

    template<typename, typename, typename, typename>
    friend
    class entt::basic_storage;

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
    decltype(auto) get() const;

    private:
    ChaosEntity(entt::entity entity);

    entt::entity entity{entt::null};
    ChaosRegistry *registry{nullptr};
  };

  template<typename T>
  concept IsChaosEntity = std::is_same_v<T, SC::ChaosEntity>;

  template<typename T>
  concept Mutable = !std::is_const_v<T>;

  DEFINE_CHAOS_STAT(Entities, "Entities", SC::ChaosThroughput<SC::MetricUnits>, false);
  REGISTER_CHAOS_STAT(Entities)


  class ChaosRegistry {
    template<typename Component>
    struct alignas(64) ComponentAccess {
      mutable std::shared_mutex mutex;
      std::pmr::synchronized_pool_resource pool;
      bool created = false;
    };

    static constexpr uint32_t ENTITY_BLOCK_SIZE = 512;
    using entity = entt::entity;

    std::vector<std::pmr::synchronized_pool_resource *> m_pools{};

    public:
    ~ChaosRegistry() {
      // m_reg.clear();
      for (auto pool: m_pools) {
        pool->release();
      }
    }

    ChaosEntity create();

    template<typename Component>
    void reserve(size_t count) {
      std::unique_lock lock(m_regMutex);
      auto &s = m_reg.storage<Component>();
      s.reserve(count);
    }

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
    void erase(entt::entity e) {
      executeWrite<Component>([&]() {
        m_reg.erase<Component>(e);
      });
    }

    template<typename... Components>
    decltype(auto) get(entt::entity e);

    template<typename... Components>
    decltype(auto) cget(entt::entity e) const;

    template<typename... Components>
    decltype(auto) view();


    private:
    void createEntities();

    template<typename Component>
    auto *getComponentAccess();

    template<typename Component>
    auto *getComponentAccess() const;

    template<typename Component>
    void connectOnDestroy();

    template<typename Component, typename Func>
    decltype(auto) executeWrite(Func &&func);

    template<Mutable... Component, typename Func>
    decltype(auto) executeRead(Func &&func);

    template<typename... Component, typename Func>
    decltype(auto) cexecuteRead(Func &&func) const;


    template<typename Component>
    static void cleanupResources(entt::registry &reg, entt::entity e);

    alignas(64) mutable std::shared_mutex m_regMutex;
    alignas(64) std::mutex eCreationLock;
    alignas(64) std::atomic<uint32_t> mEntityIdx{ENTITY_BLOCK_SIZE};
    entt::registry m_reg;
    std::array<ChaosEntity, ENTITY_BLOCK_SIZE> mEntities{};
  };


  template<typename It>
  requires IsChaosEntity<typename std::iterator_traits<It>::value_type>
  void ChaosRegistry::create(It begin, It end) {
    std::lock_guard lock{eCreationLock};
    {
      std::shared_lock guard{m_regMutex};
      m_reg.create(begin, end);
    }
    for (auto iter = begin; iter != end; ++iter) {
      iter->registry = this;
    }
    CHAOS_RECORD(Entities, std::distance(begin, end))
  }

  template<typename... Components>
  decltype(auto) ChaosRegistry::get(entt::entity e) {
    return executeRead<Components...>([&]() -> decltype(auto) {
      return m_reg.get<Components...>(e);
    });
  }

  template<typename... Components>
  decltype(auto) ChaosRegistry::cget(entt::entity e) const {
    return cexecuteRead<Components...>([&]() -> decltype(auto) {
      return m_reg.get<const Components...>(e);
    });
  }

  template<typename ... Components>
  decltype(auto) ChaosRegistry::view() {
    return executeRead<Components ...>([&]() -> decltype(auto) {
      return m_reg.view<Components ...>();
    });
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

  template<typename Component>
  auto *ChaosRegistry::getComponentAccess() const {
    using AccessType = ComponentAccess<Component>;

    {
      std::shared_lock lock(m_regMutex);
      if (auto *uptr = m_reg.ctx().find<std::unique_ptr<AccessType>>()) {
        return uptr->get();
      }
    }

    std::unique_lock lock(m_regMutex);

    if (auto *uptr = m_reg.ctx().find<std::unique_ptr<AccessType>>()) {
      return uptr->get();
    }

    auto &mutableReg = const_cast<entt::registry &>(m_reg);

    auto uptr = std::make_unique<AccessType>();
    auto *ptr = uptr.get();
    mutableReg.ctx().emplace<std::unique_ptr<AccessType>>(std::move(uptr));

    const_cast<std::vector<std::pmr::synchronized_pool_resource *> &>(m_pools).emplace_back(&ptr->pool);

    return ptr;
  }

  template<typename Component>
  void ChaosRegistry::connectOnDestroy() {
    constexpr bool has_resource = []() {
      if constexpr (!pfr::is_implicitly_reflectable_v<Component, void>) {
        return false;
      }
      else {
        bool found = false;
        pfr::for_each_field(Component{}, [&](auto &&field) {
          using FieldType = std::remove_cvref_t<decltype(field)>;
          if constexpr (IsChaosResource<FieldType>) {
            found = true;
          }
        });
        return found;
      }
    }();

    if constexpr (has_resource) {
      m_reg.on_destroy<Component>().template connect<&ChaosRegistry::cleanupResources<Component>>();
    }
  }

  template<typename Component>
  void ChaosRegistry::cleanupResources(entt::registry &reg, entt::entity e) {
    auto &comp = reg.get<Component>(e);
    pfr::for_each_field(comp, [](auto &field) {
      using FieldType = std::remove_cvref_t<decltype(field)>;
      if constexpr (IsChaosResource<FieldType>) {
        field.clear();
      }
    });
  }


  template<typename Component, typename Func>
  decltype(auto) ChaosRegistry::executeWrite(Func &&func) {
    auto *acc = getComponentAccess<Component>();
    static_assert(std::is_move_constructible_v<Component>, "Component must be moveable");
    static_assert(std::is_nothrow_move_constructible_v<Component>, "Move must be no-throw for EnTT performance");
    static_assert(std::is_trivially_destructible_v<Component>);

    PoolAnchor::current = &acc->pool;
    std::unique_lock lock(acc->mutex);
    if (!acc->created) [[unlikely]] {
      std::unique_lock regLock(m_regMutex);
      if (!acc->created) {
        connectOnDestroy<Component>();
        if constexpr (std::is_void_v<std::invoke_result_t<Func> >) {
          func();
          acc->created = true;
          PoolAnchor::current = nullptr;
          return;
        }
        else {
          decltype(auto) ret = func();
          PoolAnchor::current = nullptr;

          acc->created = true;
          return ret;
        }
      }
    }
    std::shared_lock regLock{m_regMutex};
    if constexpr (std::is_void_v<std::invoke_result_t<Func> >) {
      func();
      acc->created = true;
      PoolAnchor::current = nullptr;
    }
    else {
      decltype(auto) ret = func();
      PoolAnchor::current = nullptr;
      acc->created = true;
      return ret;
    }
  }

  template<typename>
  using AlwaysLock = std::shared_lock<std::shared_mutex>;

  template<typename... Components>
  struct [[nodiscard]] ChaosLock {
    std::tuple<AlwaysLock<Components>...> locks;

    ChaosLock(std::tuple<AlwaysLock<Components>...> &&l)
        : locks(std::move(l)) {
      lock();
    }

    ChaosLock(ChaosLock &&other) noexcept: locks(std::move(other.locks)) {
    }

    void lock() {
      std::apply([](auto &... lk) {
        if constexpr (sizeof...(lk) > 1) std::lock(lk...);
        else if constexpr (sizeof...(lk) == 1) (lk.lock(), ...);
      }, locks);
    }

    void unlock() {
      std::apply([](auto &... lk) { (lk.unlock(), ...); }, locks);
    }
  };


  template<Mutable... Components, typename Func>
  decltype(auto) ChaosRegistry::executeRead(Func &&func) {

    std::shared_lock registryLock(m_regMutex);
    auto accessors = std::make_tuple(getComponentAccess<Components>()...);
    ChaosLock<Components...> lock{
        std::make_tuple(
            std::shared_lock{std::get<ComponentAccess<Components> *>(accessors)->mutex, std::defer_lock}...)
    };

    if constexpr (std::is_void_v<std::invoke_result_t<Func> >) {
      func();
    }
    else {
      decltype(auto) result = func();

      if constexpr (entt::is_tuple_v<std::decay_t<decltype(result)>>) {
        return std::apply([&]<typename... T0>(T0 &&... args) {
          return std::tuple<ChaosLock<Components...>, T0...>(
              std::move(lock), std::forward<T0>(args)...
          );
        }, std::move(result));
      }
      else {
        using CompRef = decltype(result);
        return std::tuple<ChaosLock<Components...>, CompRef>(
            std::move(lock), std::forward<CompRef>(result)
        );
      }
    }
  }

  template<typename... Components, typename Func>
  decltype(auto) ChaosRegistry::cexecuteRead(Func &&func) const {
    auto accessors = std::make_tuple(getComponentAccess<Components>()...);
    ChaosLock<Components...> lock{
        std::make_tuple(
            std::shared_lock{std::get<ComponentAccess<Components> *>(accessors)->mutex, std::defer_lock}...)
    };
    std::shared_lock registryLock(m_regMutex);

    if constexpr (std::is_void_v<std::invoke_result_t<Func> >) {
      func();
    }
    else {
      decltype(auto) result = func();

      if constexpr (entt::is_tuple_v<std::decay_t<decltype(result)>>) {
        return std::apply([&]<typename... T0>(T0 &&... args) {
          return std::tuple<ChaosLock<Components...>, T0...>(
              std::move(lock), std::forward<T0>(args)...
          );
        }, std::move(result));
      }
      else {
        using CompRef = decltype(result);
        return std::tuple<ChaosLock<Components...>, CompRef>(
            std::move(lock), std::forward<CompRef>(result)
        );
      }
    }
  }


  template<typename T, typename... Args>
  decltype(auto) ChaosEntity::add(Args &&... args) {
    return registry->emplace<T>(entity, std::forward<Args>(args)...);
  }

  template<typename T>
  decltype(auto) ChaosEntity::get() const {
    return registry->get<T>(entity);
  }


} // SC
