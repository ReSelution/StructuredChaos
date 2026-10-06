#pragma once

#include "entt/entt.hpp"
#include "resource.hpp"

#include <entt/entity/fwd.hpp>
#include <array>
#include <cstddef>
#include <memory>
#include <mimalloc.h>
#include <mutex>
#include <stdexcept>

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

// Upper bound for the number of distinct component types in the program.
inline constexpr size_t MAX_COMPONENT_TYPES = 1024;

inline std::atomic<size_t> nextComponentIndex{0};

// Index of a component type, the same in every registry. Assigned on first
// use, counted over all component types of the program. Not stable across
// shared library boundaries, where every module would count on its own.
template <typename Component> size_t componentIndex() {
  static const size_t index = [] {
    const size_t next =
        nextComponentIndex.fetch_add(1, std::memory_order_relaxed);
    if (next >= MAX_COMPONENT_TYPES) {
      throw std::length_error(
          "sc::ecs: more component types than MAX_COMPONENT_TYPES");
    }
    return next;
  }();
  return index;
}

} // namespace sc::ecs::internal

namespace sc::ecs {
using Entities =
    sc::stats::Stat<"Entities", sc::stats::Throughput<sc::stats::MetricUnits>>;

template <typename T>
concept Mutable = !std::is_const_v<T>;
class Registry {
  template <typename Component>
  using StorageFor = entt::storage_for_t<std::remove_const_t<Component>>;

  // Whether a component has Resource fields, which own memory outside of the
  // component storage.
  template <typename Component>
  static constexpr bool hasResource = []() {
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

  template <typename Component> struct alignas(64) ComponentAccess {
    mutable std::shared_mutex mutex;
    // Heap the Resource fields of the component allocate from, null for a
    // component without any. Components are trivially destructible, so their
    // buffers are not freed one by one: destroying the heap frees them all.
    mi_heap_t *heap = nullptr;
    // Set once on creation. All component operations go through this pointer
    // instead of m_reg, which would look up the pool table shared by all
    // components on every call.
    StorageFor<Component> *storage = nullptr;

    ~ComponentAccess() {
      if (heap != nullptr) {
        mi_heap_destroy(heap);
      }
    }
  };

  static constexpr uint32_t ENTITY_BLOCK_SIZE = 512;

  using entity = entt::entity;

public:
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
    HeapAnchor::current = acc->heap;
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
  void refillEntities();

  template <typename Component> auto *getComponentAccess();

  template <typename Component> auto *getComponentAccess() const;

  template <typename Component> auto *findOrCreateComponentAccess() const;

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

  // The access object of every component type used with this registry, at
  // internal::componentIndex<Component>(). Read without a lock by all threads;
  // an entry is written once, under m_contextMutex, and never changes after.
  mutable std::array<std::atomic<void *>, internal::MAX_COMPONENT_TYPES>
      m_access{};

  // Serializes the creation of access objects.
  alignas(64) mutable std::mutex m_contextMutex;
  // Guards m_reg's entity storage and the refilling of m_entities.
  alignas(64) std::mutex m_entityMutex;

  // Entities created in advance for create(), handed out one index at a time
  // and refilled in place once all of them are gone.
  //
  // m_entityNext is the next index to hand out. m_entityRead counts the
  // entities of the current round that were copied out: a thread takes its
  // index first and reads the entity after, so the buffer may only be
  // overwritten once every index that was handed out has also been read.
  alignas(64) std::atomic<uint32_t> m_entityNext{ENTITY_BLOCK_SIZE};
  alignas(64) std::atomic<uint32_t> m_entityRead{ENTITY_BLOCK_SIZE};
  alignas(64) std::array<Entity, ENTITY_BLOCK_SIZE> m_entities{};

  mutable entt::registry m_reg;
};

template <typename It>
  requires IsEntity<typename std::iterator_traits<It>::value_type>
void Registry::create(It begin, It end) {

  {
    std::lock_guard lock{m_entityMutex};
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
  // A const-qualified component would get an access object, and with it a
  // mutex, of its own for the same storage.
  static_assert(!std::is_const_v<Component>);
  using AccessType = ComponentAccess<Component>;

  void *access = m_access[internal::componentIndex<Component>()].load(
      std::memory_order_acquire);
  if (access == nullptr) [[unlikely]] {
    return findOrCreateComponentAccess<Component>();
  }
  return static_cast<AccessType *>(access);
}

template <typename Component>
auto *Registry::findOrCreateComponentAccess() const {
  using AccessType = ComponentAccess<Component>;
  std::atomic<void *> &slot = m_access[internal::componentIndex<Component>()];

  std::lock_guard lock(m_contextMutex);
  if (void *access = slot.load(std::memory_order_relaxed)) {
    return static_cast<AccessType *>(access);
  }

  // The only place that touches the pool table of m_reg, serialized by the
  // context lock. EnTT keeps a storage at a stable address.
  auto &storage = m_reg.storage<std::remove_const_t<Component>>();
  connectOnDestroy<Component>(storage);

  auto uptr = std::make_unique<AccessType>();
  auto *ptr = uptr.get();
  ptr->storage = &storage;
  if constexpr (hasResource<std::remove_const_t<Component>>) {
    ptr->heap = mi_heap_new();
  }
  // m_reg owns the access object; the slot makes it visible to all threads.
  m_reg.ctx().emplace<std::unique_ptr<AccessType>>(std::move(uptr));
  slot.store(ptr, std::memory_order_release);

  return ptr;
}

template <typename Component>
void Registry::connectOnDestroy(StorageFor<Component> &storage) {
  if constexpr (hasResource<std::remove_const_t<Component>>) {
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

  HeapAnchor::current = acc->heap;
  std::unique_lock lock(acc->mutex);
  if constexpr (std::is_void_v<
                    std::invoke_result_t<Func, StorageFor<Component> &>>) {
    func(*acc->storage);
    HeapAnchor::current = nullptr;
    return;
  } else {
    decltype(auto) ret = func(*acc->storage);
    HeapAnchor::current = nullptr;
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
