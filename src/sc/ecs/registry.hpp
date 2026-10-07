#pragma once

#include "entt/entt.hpp"
#include "resource.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <entt/entity/fwd.hpp>
#include <functional>
#include <memory>
#include <mimalloc.h>
#include <mutex>
#include <stdexcept>

#include <atomic>
#include <utility>
#include <vector>

#include <pfr.hpp>

#include "component_lock.hpp"
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

// Upper bound for the number of distinct group types in the program.
inline constexpr size_t MAX_GROUP_TYPES = 256;

inline std::atomic<size_t> nextGroupIndex{0};

// Index of a group type, assigned like componentIndex.
template <typename Group> size_t groupIndex() {
  static const size_t index = [] {
    const size_t next = nextGroupIndex.fetch_add(1, std::memory_order_relaxed);
    if (next >= MAX_GROUP_TYPES) {
      throw std::length_error("sc::ecs: more group types than MAX_GROUP_TYPES");
    }
    return next;
  }();
  return index;
}

// The part of a component's access object that does not depend on its type.
struct ComponentAccessBase {
  ComponentLock ownLock;
  // The lock that guards the component. It is the component's own one until
  // the component becomes part of a group: a group reorders the storages of
  // its components whenever one of them changes, so they all have to share a
  // single lock. Only changed while the old lock is held exclusively.
  std::atomic<ComponentLock *> lock{&ownLock};
  // The component's storage without its type, for operations that have to
  // visit every component of a registry.
  entt::sparse_set *storageBase = nullptr;
};

// Takes the lock of a component exclusively and returns it. The lock may be
// replaced while a thread waits for it, hence the check afterwards.
inline ComponentLock *lockExclusive(ComponentAccessBase &access) noexcept {
  while (true) {
    ComponentLock *lock = access.lock.load(std::memory_order_acquire);
    lock->lock();
    if (access.lock.load(std::memory_order_acquire) == lock) [[likely]] {
      return lock;
    }
    lock->unlock();
  }
}

} // namespace sc::ecs::internal

namespace sc::ecs {
using Entities =
    sc::stats::Stat<"Entities", sc::stats::Throughput<sc::stats::MetricUnits>>;

template <typename T>
concept Mutable = !std::is_const_v<T>;

// Shared locks on several components, held for as long as the object lives.
//
// While a thread holds one, it must not ask for another lock on the same
// components (a second view, a get, a group that contains them): a writer
// waiting in between would block the second request for good.
//
// The locks are always taken in the order of their addresses. Two readers
// that name the same components in a different order therefore cannot block
// each other through a writer waiting in between, and no back-off algorithm
// like the one of std::lock is needed.
template <typename... Components> struct [[nodiscard]] Lock {
  static constexpr size_t COUNT = sizeof...(Components);

  std::array<internal::ComponentAccessBase *, COUNT> accesses;
  // The locks that are held, in the order they were taken.
  std::array<internal::ComponentLock *, COUNT> mutexes{};
  bool owns = false;

  explicit Lock(std::array<internal::ComponentAccessBase *, COUNT> a)
      : accesses(a) {
    lock();
  }

  Lock(Lock &&other) noexcept
      : accesses(other.accesses), mutexes(other.mutexes),
        owns(std::exchange(other.owns, false)) {}

  Lock &operator=(Lock &&other) noexcept {
    if (this != &other) {
      if (owns) {
        unlock();
      }
      accesses = other.accesses;
      mutexes = other.mutexes;
      owns = std::exchange(other.owns, false);
    }
    return *this;
  }

  Lock(const Lock &) = delete;
  Lock &operator=(const Lock &) = delete;

  ~Lock() {
    if (owns) {
      unlock();
    }
  }

  void lock() {
    while (true) {
      std::array<internal::ComponentLock *, COUNT> wanted;
      for (size_t i = 0; i < COUNT; ++i) {
        wanted[i] = accesses[i]->lock.load(std::memory_order_acquire);
      }

      mutexes = wanted;
      if constexpr (COUNT == 2) {
        if (std::less<>{}(mutexes[1], mutexes[0])) {
          std::swap(mutexes[0], mutexes[1]);
        }
      } else if constexpr (COUNT > 2) {
        std::sort(mutexes.begin(), mutexes.end(), std::less<>{});
      }
      // Components of one group share a lock. It shows up several times in
      // a row then and must only be taken once.
      for (size_t i = 0; i < COUNT; ++i) {
        if (i == 0 || mutexes[i] != mutexes[i - 1]) {
          mutexes[i]->lock_shared();
        }
      }

      // A component may have been moved to another lock while this thread
      // waited for the old one.
      bool current = true;
      for (size_t i = 0; i < COUNT; ++i) {
        current = current &&
                  accesses[i]->lock.load(std::memory_order_acquire) == wanted[i];
      }
      if (current) [[likely]] {
        owns = true;
        return;
      }
      release();
    }
  }

  void unlock() {
    release();
    owns = false;
  }

private:
  void release() {
    for (size_t i = COUNT; i-- > 0;) {
      if (i == 0 || mutexes[i] != mutexes[i - 1]) {
        mutexes[i]->unlock_shared();
      }
    }
  }
};

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

  template <typename Component>
  struct alignas(64) ComponentAccess : internal::ComponentAccessBase {
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
    executeWrite<Component>(
        [&](auto &storage) -> void { storage.insert(first, last, dataBegin); });
  }

  template <typename Component> void erase(entt::entity e) {
    executeWrite<Component>([&](auto &storage) { storage.erase(e); });
  }

  // Whether the entity exists, that is, was created and not destroyed since.
  bool valid(entt::entity e) const;

  // Removes the entity together with all of its components. Returns false if
  // the entity does not exist (any more).
  //
  // Takes every component of the registry exclusively, one after the other,
  // so it must not be called while holding a lock of this registry. Nothing
  // else may use the entity while it is being destroyed. Its number is handed
  // out again by a later create, with a new version: handles to the destroyed
  // entity stay invalid.
  bool destroy(entt::entity e);

  // The same for a range of entities, taking each component only once.
  template <typename It> void destroy(It first, It last);

  template <typename... Components> decltype(auto) get(entt::entity e);

  template <typename... Components> decltype(auto) cget(entt::entity e) const;

  template <typename... Components>

  auto view();

  // Returns the group of the given components, like entt::registry::group,
  // together with a shared lock that is held for as long as the result lives.
  //
  // The first call for a group creates it. That takes the components
  // exclusively and, from then on, makes them share one lock: writing one of
  // them blocks access to all of them. Do not make that first call while
  // holding a lock of this registry (a view, a group, a reference from get).
  template <typename... Owned, typename... Get, typename... Exclude>
  auto group(entt::get_t<Get...> = entt::get_t{},
             entt::exclude_t<Exclude...> = entt::exclude_t{});

private:
  void refillEntities();

  template <typename Component> auto *getComponentAccess();

  template <typename Component> auto *getComponentAccess() const;

  template <typename Component> auto *findOrCreateComponentAccess() const;

  template <typename GroupType> struct GroupAccess {
    GroupType group;
    // Any component of the group; they all share one lock.
    internal::ComponentAccessBase *member;
  };

  template <typename... Owned, typename... Get, typename... Exclude>
  auto *createGroup(entt::get_t<Get...>, entt::exclude_t<Exclude...>);

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
  mutable std::array<std::atomic<internal::ComponentAccessBase *>,
                     internal::MAX_COMPONENT_TYPES>
      m_access{};

  // Like m_access for groups, at internal::groupIndex<GroupType>().
  mutable std::array<std::atomic<void *>, internal::MAX_GROUP_TYPES>
      m_groups{};

  // Every access object of this registry. Guarded by m_contextMutex.
  mutable std::vector<internal::ComponentAccessBase *> m_allAccesses;

  // Serializes the creation of groups, and with it every change of the lock
  // a component uses. Taken before any component lock and m_contextMutex.
  alignas(64) std::mutex m_groupMutex;
  // Serializes the creation of access objects and everything else that
  // changes the tables of m_reg. Taken after component locks.
  alignas(64) mutable std::mutex m_contextMutex;
  // Guards m_reg's entity storage and the refilling of m_entities.
  alignas(64) mutable std::mutex m_entityMutex;

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

template <typename It> void Registry::destroy(It first, It last) {
  const size_t componentCount =
      std::min(internal::nextComponentIndex.load(std::memory_order_acquire),
               internal::MAX_COMPONENT_TYPES);
  for (size_t i = 0; i < componentCount; ++i) {
    internal::ComponentAccessBase *access =
        m_access[i].load(std::memory_order_acquire);
    if (access == nullptr) {
      continue; // this registry does not use the component
    }
    std::unique_lock<internal::ComponentLock> lock(
        *internal::lockExclusive(*access), std::adopt_lock);
    for (It it = first; it != last; ++it) {
      access->storageBase->remove(static_cast<entt::entity>(*it));
    }
  }

  std::lock_guard lock{m_entityMutex};
  auto &entities = m_reg.storage<entt::entity>();
  for (It it = first; it != last; ++it) {
    const auto e = static_cast<entt::entity>(*it);
    if (m_reg.valid(e)) {
      entities.erase(e);
    }
  }
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

template <typename... Owned, typename... Get, typename... Exclude>
auto Registry::group(entt::get_t<Get...> get, entt::exclude_t<Exclude...> exclude) {
  using GroupType = decltype(m_reg.group<Owned...>(get, exclude));

  void *found = m_groups[internal::groupIndex<GroupType>()].load(
      std::memory_order_acquire);
  auto *access = found != nullptr
                     ? static_cast<GroupAccess<GroupType> *>(found)
                     : createGroup<Owned...>(get, exclude);

  Lock<GroupType> lock{{access->member}};
  return View{std::move(lock), access->group};
}

template <typename... Owned, typename... Get, typename... Exclude>
auto *Registry::createGroup(entt::get_t<Get...> get,
                            entt::exclude_t<Exclude...> exclude) {
  using GroupType = decltype(m_reg.group<Owned...>(get, exclude));
  constexpr size_t COUNT =
      sizeof...(Owned) + sizeof...(Get) + sizeof...(Exclude);

  // Before any lock is taken: creating an access object takes m_contextMutex.
  const std::array<internal::ComponentAccessBase *, COUNT> members{
      getComponentAccess<Owned>()..., getComponentAccess<Get>()...,
      getComponentAccess<Exclude>()...};
  std::atomic<void *> &slot = m_groups[internal::groupIndex<GroupType>()];

  std::lock_guard groupLock(m_groupMutex);
  if (void *existing = slot.load(std::memory_order_relaxed)) {
    return static_cast<GroupAccess<GroupType> *>(existing);
  }

  // The locks the components use right now. They cannot change underneath:
  // only this function changes them, and it holds m_groupMutex.
  std::array<internal::ComponentLock *, COUNT> locks;
  for (size_t i = 0; i < COUNT; ++i) {
    locks[i] = members[i]->lock.load(std::memory_order_relaxed);
  }
  std::sort(locks.begin(), locks.end(), std::less<>{});
  const auto locksEnd = std::unique(locks.begin(), locks.end());

  for (auto it = locks.begin(); it != locksEnd; ++it) {
    (*it)->lock();
  }

  GroupAccess<GroupType> *access = nullptr;
  {
    std::lock_guard contextLock(m_contextMutex);

    // Creates the group in m_reg: connects it to the storages and brings
    // their current content into the order of the group.
    auto owner = std::make_unique<GroupAccess<GroupType>>(
        m_reg.group<Owned...>(get, exclude), members[0]);
    access = owner.get();
    m_reg.ctx().emplace<std::unique_ptr<GroupAccess<GroupType>>>(
        std::move(owner));

    // From now on a change of one component touches the others as well, so
    // every component guarded by one of the locks moves to a single lock.
    // That includes components of earlier groups that overlap with this one.
    internal::ComponentLock *const shared = locks[0];
    for (internal::ComponentAccessBase *component : m_allAccesses) {
      internal::ComponentLock *const current =
          component->lock.load(std::memory_order_relaxed);
      if (current != shared &&
          std::find(locks.begin(), locksEnd, current) != locksEnd) {
        component->lock.store(shared, std::memory_order_release);
      }
    }

    slot.store(access, std::memory_order_release);
  }

  for (auto it = locksEnd; it != locks.begin();) {
    (*--it)->unlock();
  }
  return access;
}

template <typename Component> auto *Registry::getComponentAccess() {
  return std::as_const(*this).template getComponentAccess<Component>();
}

template <typename Component> auto *Registry::getComponentAccess() const {
  // A const-qualified component would get an access object, and with it a
  // mutex, of its own for the same storage.
  static_assert(!std::is_const_v<Component>);
  using AccessType = ComponentAccess<Component>;

  internal::ComponentAccessBase *access =
      m_access[internal::componentIndex<Component>()].load(
          std::memory_order_acquire);
  if (access == nullptr) [[unlikely]] {
    return findOrCreateComponentAccess<Component>();
  }
  return static_cast<AccessType *>(access);
}

template <typename Component>
auto *Registry::findOrCreateComponentAccess() const {
  using AccessType = ComponentAccess<Component>;
  std::atomic<internal::ComponentAccessBase *> &slot =
      m_access[internal::componentIndex<Component>()];

  std::lock_guard lock(m_contextMutex);
  if (internal::ComponentAccessBase *access =
          slot.load(std::memory_order_relaxed)) {
    return static_cast<AccessType *>(access);
  }

  // The only place that touches the pool table of m_reg, serialized by the
  // context lock. EnTT keeps a storage at a stable address.
  auto &storage = m_reg.storage<std::remove_const_t<Component>>();
  connectOnDestroy<Component>(storage);

  auto uptr = std::make_unique<AccessType>();
  auto *ptr = uptr.get();
  ptr->storage = &storage;
  ptr->storageBase = &storage;
  if constexpr (hasResource<std::remove_const_t<Component>>) {
    ptr->heap = mi_heap_new();
  }
  // m_reg owns the access object; the slot makes it visible to all threads.
  m_reg.ctx().emplace<std::unique_ptr<AccessType>>(std::move(uptr));
  m_allAccesses.push_back(ptr);
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
  std::unique_lock<internal::ComponentLock> lock(
      *internal::lockExclusive(*acc), std::adopt_lock);
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

template <Mutable... Components, typename Func>
decltype(auto) Registry::executeRead(Func &&func) {

  auto accessors = std::make_tuple(getComponentAccess<Components>()...);
  Lock<Components...> lock{
      {std::get<ComponentAccess<Components> *>(accessors)...}};

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
  Lock<Components...> lock{
      {std::get<ComponentAccess<Components> *>(accessors)...}};

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
