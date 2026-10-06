#pragma once

#include "entt/entt.hpp"
#include "resource.hpp"

#include <entt/entity/fwd.hpp>
#include <memory_resource>
#include <mutex>

#include <atomic>
#include <shared_mutex>
#include <utility>

#include <pfr.hpp>

#include "entity.hpp"
#include "view.hpp"

#include "sc/stats/stats.hpp"
#include "sc/stats/throughput.hpp"

#include "anchor.hpp"

namespace sc::ecs::internal {
template <typename It>
  requires IsEntity<typename std::iterator_traits<It>::value_type>
struct EntityOutputIterator {
  It current;
  Registry *reg;

  using value_type = void;
  using difference_type = std::ptrdiff_t;
  using pointer = void;
  using reference = void;
  using iterator_category = std::output_iterator_tag;

  EntityOutputIterator &operator*() { return *this; }
  EntityOutputIterator &operator++() { return *this; }
  EntityOutputIterator operator++(int) { return *this; }

  EntityOutputIterator &operator=(entt::entity e) {
    *current = Entity{e, reg};
    ++current;
    return *this;
  }

  operator entt::entity() const { return *current; }

  bool operator==(const EntityOutputIterator &other) const {
    return current == other.current;
  }

  bool operator!=(const EntityOutputIterator &other) const {
    return !(*this == other);
  }
};

} // namespace sc::ecs::internal

namespace sc::ecs {
using Entities =
    sc::stats::Stat<"Entities", sc::stats::Throughput<sc::stats::MetricUnits>>;

template <typename T>
concept Mutable = !std::is_const_v<T>;
class Registry {
  template <typename Component>
  using StorageFor = entt::storage_for_t<std::remove_const_t<Component>>;

  template <typename Component> struct alignas(64) ComponentAccess {
    mutable std::shared_mutex mutex;
    std::pmr::synchronized_pool_resource pool;
    // Set once on creation. All component operations go through this pointer
    // instead of m_reg, which would look up the pool table shared by all
    // components on every call.
    StorageFor<Component> *storage = nullptr;
  };

  static constexpr uint32_t ENTITY_BLOCK_SIZE = 512;
  using entity = entt::entity;

  mutable std::vector<std::pmr::synchronized_pool_resource *> m_pools{};

public:
  ~Registry() {
    // m_reg.clear();
    for (auto pool : m_pools) {
      pool->release();
    }
  }

  Entity create();

  template <typename Component> void reserve(size_t count) {
    executeWrite<Component>(
        [&](auto &storage) -> void { storage.reserve(count); });
  }

  template <typename It>
    requires IsEntity<typename std::iterator_traits<It>::value_type>
  void create(It begin, It end);

  template <typename Component, typename... Args>
  decltype(auto) emplace(const entity e, Args &&...args) {
    return executeWrite<Component>([&](auto &storage) -> decltype(auto) {
      return storage.emplace(e, std::forward<Args>(args)...);
    });
  }

  template <typename Component> void setStorageAnchor() {
    auto *acc = getComponentAccess<Component>();
    PoolAnchor::current = &acc->pool;
  }

  template <typename Component, typename It, typename DataIt>
  void insert(It first, It last, DataIt dataBegin) {
    executeWrite<Component>([&](auto &storage) -> void {
      storage.insert(first, last, dataBegin);
    });
  }

  template <typename Component> void erase(entt::entity e) {
    executeWrite<Component>([&](auto &storage) { storage.erase(e); });
  }

  template <typename... Components> decltype(auto) get(entt::entity e);

  template <typename... Components> decltype(auto) cget(entt::entity e) const;

  template <typename... Components>

  auto view();

private:
  void createEntities();

  template <typename Component> auto *getComponentAccess();

  template <typename Component> auto *getComponentAccess() const;

  template <typename Component>
  static void connectOnDestroy(StorageFor<Component> &storage);

  template <typename Component, typename Func>
  decltype(auto) executeWrite(Func &&func);

  template <Mutable... Component, typename Func>
  decltype(auto) executeRead(Func &&func);

  template <typename... Component, typename Func>
  decltype(auto) cexecuteRead(Func &&func) const;

  template <typename Component>
  static void cleanupResources(StorageFor<Component> &storage,
                               entt::registry &reg, entt::entity e);

  alignas(64) mutable std::shared_mutex m_contextMutex;
  // Guards entity creation: exclusive while m_reg creates entities and
  // mEntities is refilled, shared while an entity is handed out.
  alignas(64) mutable std::shared_mutex m_entityMutex;
  alignas(64) std::atomic<uint32_t> mEntityIdx{ENTITY_BLOCK_SIZE};
  mutable entt::registry m_reg;
  std::array<Entity, ENTITY_BLOCK_SIZE> mEntities{};
};

template <typename It>
  requires IsEntity<typename std::iterator_traits<It>::value_type>
void Registry::create(It begin, It end) {

  {
    std::unique_lock lock{m_entityMutex};
    m_reg.create(internal::EntityOutputIterator{begin, this},
                 internal::EntityOutputIterator{end, this});
  }

  Entities::record(std::distance(begin, end));
}

template <typename... Components> decltype(auto) Registry::get(entt::entity e) {
  return executeRead<Components...>([&](auto &...storage) -> decltype(auto) {
    if constexpr (sizeof...(storage) == 1) {
      return (storage.get(e), ...);
    } else {
      return std::forward_as_tuple(storage.get(e)...);
    }
  });
}

template <typename... Components>
decltype(auto) Registry::cget(entt::entity e) const {
  return cexecuteRead<Components...>(
      [&](const auto &...storage) -> decltype(auto) {
        if constexpr (sizeof...(storage) == 1) {
          return (storage.get(e), ...);
        } else {
          return std::forward_as_tuple(storage.get(e)...);
        }
      });
}

template <typename... Components> auto Registry::view() {
  return View{executeRead<Components...>(
      [](auto &...storage) { return entt::basic_view{storage...}; })};
}

template <typename Component> auto *Registry::getComponentAccess() {
  return std::as_const(*this).template getComponentAccess<Component>();
}

template <typename Component> auto *Registry::getComponentAccess() const {
  using AccessType = ComponentAccess<Component>;
  {
    std::shared_lock shared_lock(m_contextMutex);
    if (auto *uptr = m_reg.ctx().find<std::unique_ptr<AccessType>>()) {
      return uptr->get();
    }
  }

  std::unique_lock unique_lock(m_contextMutex);
  if (auto *uptr = m_reg.ctx().find<std::unique_ptr<AccessType>>()) {
    return uptr->get();
  }

  // The only place that touches the pool table of m_reg, serialized by the
  // exclusive context lock. EnTT keeps a storage at a stable address.
  auto &storage = m_reg.storage<std::remove_const_t<Component>>();
  connectOnDestroy<Component>(storage);

  auto uptr = std::make_unique<AccessType>();
  auto *ptr = uptr.get();
  ptr->storage = &storage;
  m_reg.ctx().emplace<std::unique_ptr<AccessType>>(std::move(uptr));

  m_pools.emplace_back(&ptr->pool);

  return ptr;
}

template <typename Component>
void Registry::connectOnDestroy(StorageFor<Component> &storage) {
  constexpr bool has_resource = []() {
    if constexpr (!pfr::is_implicitly_reflectable_v<Component, void>) {
      return false;
    } else {
      bool found = false;
      pfr::for_each_field(Component{}, [&](auto &&field) {
        using FieldType = std::remove_cvref_t<decltype(field)>;
        if constexpr (IsResource<FieldType>) {
          found = true;
        }
      });
      return found;
    }
  }();

  if constexpr (has_resource) {
    storage.on_destroy()
        .template connect<&Registry::cleanupResources<Component>>(storage);
  }
}

template <typename Component>
void Registry::cleanupResources(StorageFor<Component> &storage,
                                entt::registry &, entt::entity e) {
  auto &comp = storage.get(e);
  pfr::for_each_field(comp, [](auto &field) {
    using FieldType = std::remove_cvref_t<decltype(field)>;
    if constexpr (IsResource<FieldType>) {
      field.clear();
    }
  });
}

template <typename Component, typename Func>
decltype(auto) Registry::executeWrite(Func &&func) {
  auto *acc = getComponentAccess<Component>();
  static_assert(std::is_move_constructible_v<Component>,
                "Component must be moveable");
  static_assert(std::is_nothrow_move_constructible_v<Component>,
                "Move must be no-throw for EnTT performance");
  static_assert(std::is_trivially_destructible_v<Component>);

  PoolAnchor::current = &acc->pool;
  std::unique_lock lock(acc->mutex);
  if constexpr (std::is_void_v<
                    std::invoke_result_t<Func, StorageFor<Component> &>>) {
    func(*acc->storage);
    PoolAnchor::current = nullptr;
    return;
  } else {
    decltype(auto) ret = func(*acc->storage);
    PoolAnchor::current = nullptr;
    return ret;
  }
}

template <typename> using AlwaysLock = std::shared_lock<std::shared_mutex>;

template <typename... Components> struct [[nodiscard]] Lock {
  std::tuple<AlwaysLock<Components>...> locks;

  explicit Lock(std::tuple<AlwaysLock<Components>...> &&l)
      : locks(std::move(l)) {
    lock();
  }

  Lock(Lock &&other) noexcept = default;
  Lock &operator=(Lock &&other) noexcept = default;

  Lock(const Lock &) = delete;
  Lock &operator=(const Lock &) = delete;

  ~Lock() = default;

  void lock() {
    std::apply(
        [](auto &...lk) {
          if constexpr (sizeof...(lk) > 1) {
            std::lock(lk...);
          } else if constexpr (sizeof...(lk) == 1) {
            (lk.lock(), ...);
          }
        },
        locks);
  }

  void unlock() {
    std::apply([](auto &...lk) { (lk.unlock(), ...); }, locks);
  }
};

template <Mutable... Components, typename Func>
decltype(auto) Registry::executeRead(Func &&func) {

  auto accessors = std::make_tuple(getComponentAccess<Components>()...);
  Lock<Components...> lock{std::make_tuple(std::shared_lock{
      std::get<ComponentAccess<Components> *>(accessors)->mutex,
      std::defer_lock}...)};

  auto call = [&]() -> decltype(auto) {
    return func(
        *std::get<ComponentAccess<Components> *>(accessors)->storage...);
  };
  if constexpr (std::is_void_v<std::invoke_result_t<decltype(call)>>) {
    call();
  } else {
    decltype(auto) result = call();

    if constexpr (entt::is_tuple_v<std::decay_t<decltype(result)>>) {
      return std::apply(
          [&]<typename... T0>(T0 &&...args) {
            return std::tuple<Lock<Components...>, T0...>(
                std::move(lock), std::forward<T0>(args)...);
          },
          std::move(result));
    } else {
      using CompRef = decltype(result);
      return std::tuple<Lock<Components...>, CompRef>(
          std::move(lock), std::forward<CompRef>(result));
    }
  }
}

template <typename... Components, typename Func>
decltype(auto) Registry::cexecuteRead(Func &&func) const {
  auto accessors = std::make_tuple(getComponentAccess<Components>()...);
  Lock<Components...> lock{std::make_tuple(std::shared_lock{
      std::get<ComponentAccess<Components> *>(accessors)->mutex,
      std::defer_lock}...)};

  auto call = [&]() -> decltype(auto) {
    return func(std::as_const(
        *std::get<ComponentAccess<Components> *>(accessors)->storage)...);
  };
  if constexpr (std::is_void_v<std::invoke_result_t<decltype(call)>>) {
    call();
  } else {
    decltype(auto) result = call();

    if constexpr (entt::is_tuple_v<std::decay_t<decltype(result)>>) {
      return std::apply(
          [&]<typename... T0>(T0 &&...args) {
            return std::tuple<Lock<Components...>, T0...>(
                std::move(lock), std::forward<T0>(args)...);
          },
          std::move(result));
    } else {
      using CompRef = decltype(result);
      return std::tuple<Lock<Components...>, CompRef>(
          std::move(lock), std::forward<CompRef>(result));
    }
  }
}

template <typename T, typename... Args>
decltype(auto) Entity::add(Args &&...args) {
  return registry->emplace<T>(entity, std::forward<Args>(args)...);
}

template <typename T> decltype(auto) sc::ecs::Entity::get() const {
  return registry->get<T>(entity);
}
} // namespace sc::ecs
