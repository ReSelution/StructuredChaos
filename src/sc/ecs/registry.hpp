#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "entt/entt.hpp"
#include <entt/entity/fwd.hpp>
#include <mimalloc.h>
#include <pfr.hpp>

#include "sc/stats/stats.hpp"
#include "sc/stats/throughput.hpp"

#include "anchor.hpp"
#include "entity.hpp"
#include "resource.hpp"
#include "view.hpp"

namespace sc::ecs::internal {
  template<typename It>
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

    bool operator==(const EntityOutputIterator &other) const { return current == other.current; }

    bool operator!=(const EntityOutputIterator &other) const { return !(*this == other); }
  };

  // Upper bound for the number of distinct component types in the program.
  inline constexpr size_t MAX_COMPONENT_TYPES = 1024;

  inline std::atomic<size_t> nextComponentIndex{0};

  // Index of a component type, the same in every registry. Assigned on first
  // use, counted over all component types of the program. Not stable across
  // shared library boundaries, where every module would count on its own.
  template<typename Component>
  size_t componentIndex() {
    static const size_t index = [] {
      const size_t next = nextComponentIndex.fetch_add(1, std::memory_order_relaxed);
      if (next >= MAX_COMPONENT_TYPES) {
        throw std::length_error("sc::ecs: more component types than MAX_COMPONENT_TYPES");
      }
      return next;
    }();
    return index;
  }

  // Upper bound for the number of distinct group types in the program.
  inline constexpr size_t MAX_GROUP_TYPES = 256;

  inline std::atomic<size_t> nextGroupIndex{0};

  // Index of a group type, assigned like componentIndex.
  template<typename Group>
  size_t groupIndex() {
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
    // The components that are written together with this one, named by the
    // smallest component index among them. A group reorders the storages of
    // its components whenever an entity gains or loses one of them, so adding
    // or removing one of them writes them all. On its own until the component
    // becomes part of a group; only changed while a group is created.
    size_t family = 0;
    // The component's storage without its type, for operations that have to
    // visit every component of a registry.
    entt::sparse_set *storageBase = nullptr;
  };

  // Makes Resource fields constructed on this thread allocate from the given
  // heap for as long as the object lives.
  struct [[nodiscard]] AnchorScope {
    explicit AnchorScope(mi_heap_t *heap) noexcept { HeapAnchor::current = heap; }
    ~AnchorScope() { HeapAnchor::current = nullptr; }

    AnchorScope(const AnchorScope &) = delete;
    AnchorScope &operator=(const AnchorScope &) = delete;
  };

  // What Registry::anchor returns for a component that needs no heap.
  struct [[nodiscard]] NoAnchor {};

} // namespace sc::ecs::internal

namespace sc::ecs {
  using Entities = sc::stats::Stat<"Entities", sc::stats::Throughput<sc::stats::MetricUnits>>;

  // An EnTT registry whose components are reached without any locking.
  //
  // The registry does not synchronize access to components. Whoever uses it
  // from several threads has to make sure that a component type is either read
  // by any number of threads or written by a single one, which is what the
  // Scheduler does for the systems it runs. Different component types never
  // get in each other's way.
  //
  // Adding and removing components counts as writing, and for the components
  // of a group as writing all components of that group (see family).
  //
  // Safe to call from any thread at any time are only create and valid, and
  // the lookup of a component's storage or of a group that already exists
  // (storage, view, group), as long as what is returned is then used according
  // to the rules above.
  class Registry {
    template<typename Component>
    using StorageFor = entt::storage_for_t<std::remove_const_t<Component>>;

    // Whether a component has Resource fields, which own memory outside of the
    // component storage.
    template<typename Component>
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

    template<typename Component>
    struct ComponentAccess : internal::ComponentAccessBase {
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

  public:
    Registry();
    ~Registry();

    // Different for every registry of the program, also for one that takes the
    // place of a destroyed one in memory.
    [[nodiscard]] uint64_t id() const noexcept { return m_id; }

    Entity create();

    template<typename It>
      requires IsEntity<typename std::iterator_traits<It>::value_type>
    void create(It begin, It end);

    // Takes back entities that were created and never used: they have no
    // components and nobody else knows them. Safe to call from any thread at
    // any time, like create.
    template<typename It>
    void release(It first, It last);

    // The same for the registry with the given id, and nothing if that one
    // does not exist any more.
    template<typename It>
    static void release(uint64_t id, It first, It last);

    template<typename Component>
    void reserve(size_t count) {
      getComponentAccess<Component>()->storage->reserve(count);
    }

    template<typename Component, typename... Args>
    decltype(auto) emplace(const entt::entity e, Args &&...args) {
      [[maybe_unused]] const auto scope = anchor<Component>();
      return getComponentAccess<Component>()->storage->emplace(e, std::forward<Args>(args)...);
    }

    // Makes Resource fields constructed on this thread allocate from the heap
    // of the component, for components that are built before they are handed
    // to insert.
    template<typename Component>
    void setStorageAnchor() {
      HeapAnchor::current = getComponentAccess<Component>()->heap;
    }

    // The same for as long as the returned object lives. Does nothing for a
    // component without Resource fields, which has no heap.
    template<typename Component>
    auto anchor() {
      if constexpr (hasResource<std::remove_const_t<Component>>) {
        return internal::AnchorScope{getComponentAccess<Component>()->heap};
      } else {
        return internal::NoAnchor{};
      }
    }

    template<typename Component, typename It, typename DataIt>
    void insert(It first, It last, DataIt dataBegin) {
      [[maybe_unused]] const auto scope = anchor<Component>();
      getComponentAccess<Component>()->storage->insert(first, last, dataBegin);
    }

    template<typename Component>
    void erase(entt::entity e) {
      getComponentAccess<Component>()->storage->erase(e);
    }

    // Whether the entity exists, that is, was created and not destroyed since.
    bool valid(entt::entity e) const;

    // Removes the entity together with all of its components. Returns false if
    // the entity does not exist (any more).
    //
    // Writes every component of the registry, so nothing else may use the
    // registry in the meantime. The entity's number is handed out again by a
    // later create, with a new version: handles to the destroyed entity stay
    // invalid.
    bool destroy(entt::entity e);

    // The same for a range of entities.
    template<typename It>
    void destroy(It first, It last);

    // The component of an entity, or a tuple of references for several ones.
    template<typename... Components>
    decltype(auto) get(entt::entity e);

    // The storage of a component, const for a const component.
    template<typename Component>
    entt::storage_for_t<Component> &storage() {
      return *getComponentAccess<Component>()->storage;
    }

    // The components may name excluded ones, see View.
    template<typename... Components>
    View<Components...> view() {
      return []<typename... Get, typename... Excluded>(
                 Registry &registry,
                 std::type_identity<entt::basic_view<entt::get_t<Get...>, entt::exclude_t<Excluded...>>>) {
        return View<Components...>{
            registry.storage<entt::constness_as_t<typename Get::element_type, Get>>()...,
            registry.storage<entt::constness_as_t<typename Excluded::element_type, Excluded>>()...};
      }(*this, std::type_identity<View<Components...>>{});
    }

    // A view over the entities that have none of the excluded components.
    template<typename... Components, typename... Excluded>
    auto view(entt::exclude_t<Excluded...>) {
      using ViewType = entt::basic_view<entt::get_t<entt::storage_for_t<Components>...>,
                                        entt::exclude_t<entt::storage_for_t<Excluded>...>>;
      return ViewType{storage<Components>()..., storage<Excluded>()...};
    }

    // Returns the group of the given components, like entt::registry::group.
    //
    // The first call for a group creates it, which reorders the storages of
    // its components: nothing else may use the registry in the meantime. The
    // Scheduler does that for the groups its systems take before it runs them.
    template<typename... Owned, typename... Get, typename... Exclude>
    auto group(entt::get_t<Get...> = entt::get_t{}, entt::exclude_t<Exclude...> = entt::exclude_t{});

    // The family of the component with the given index, see
    // internal::ComponentAccessBase::family.
    [[nodiscard]] size_t family(size_t componentIndex) const noexcept {
      const internal::ComponentAccessBase *access = m_access[componentIndex].load(std::memory_order_acquire);
      return access != nullptr ? access->family : componentIndex;
    }

    // Changes whenever components are put together into a family, that is,
    // when a group is created.
    [[nodiscard]] uint64_t familyVersion() const noexcept { return m_familyVersion.load(std::memory_order_acquire); }

  private:
    void refillEntities();

    template<typename GroupType>
    struct GroupAccess {
      GroupType group;
    };

    template<typename... Owned, typename... Get, typename... Exclude>
    auto *createGroup(entt::get_t<Get...>, entt::exclude_t<Exclude...>);

    template<typename Component>
    ComponentAccess<std::remove_const_t<Component>> *getComponentAccess();

    template<typename Component>
    ComponentAccess<Component> *findOrCreateComponentAccess();

    template<typename Component>
    static void cleanupResources(StorageFor<Component> &storage, entt::registry &reg, entt::entity e);

    // The access object of every component type used with this registry, at
    // internal::componentIndex<Component>(). Read without a lock by all threads;
    // an entry is written once, under m_contextMutex, and never changes after.
    std::array<std::atomic<internal::ComponentAccessBase *>, internal::MAX_COMPONENT_TYPES> m_access{};

    // Like m_access for groups, at internal::groupIndex<GroupType>().
    std::array<std::atomic<void *>, internal::MAX_GROUP_TYPES> m_groups{};

    // Every access object of this registry. Guarded by m_contextMutex.
    std::vector<internal::ComponentAccessBase *> m_allAccesses;

    // Serializes the creation of access objects and everything else that
    // changes the tables of m_reg.
    alignas(64) std::mutex m_contextMutex;
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

    std::atomic<uint64_t> m_familyVersion{0};

    // Every registry of the program, for release to find one by its id.
    static inline std::mutex liveMutex;
    static inline std::vector<Registry *> live;

    static inline std::atomic<uint64_t> nextId{1};
    const uint64_t m_id = nextId.fetch_add(1, std::memory_order_relaxed);

    entt::registry m_reg;
  };

  template<typename It>
    requires IsEntity<typename std::iterator_traits<It>::value_type>
  void Registry::create(It begin, It end) {

    {
      std::scoped_lock lock{m_entityMutex};
      m_reg.create(internal::EntityOutputIterator{begin, this}, internal::EntityOutputIterator{end, this});
    }

    Entities::record(std::distance(begin, end));
  }

  template<typename It>
  void Registry::release(It first, It last) {
    std::scoped_lock lock{m_entityMutex};
    auto &entities = m_reg.storage<entt::entity>();
    for (; first != last; ++first) {
      entities.erase(static_cast<entt::entity>(*first));
    }
  }

  template<typename It>
  void Registry::release(uint64_t id, It first, It last) {
    // Held all the way, so that the registry is not destroyed in the meantime.
    std::scoped_lock lock{liveMutex};
    for (Registry *registry: live) {
      if (registry->m_id == id) {
        registry->release(first, last);
        return;
      }
    }
  }

  template<typename It>
  void Registry::destroy(It first, It last) {
    // Held all the way: a create that refills its buffer in between would
    // reorder the entities the iterators below point to.
    std::scoped_lock lock{m_entityMutex};
    auto &entities = m_reg.storage<entt::entity>();

    // Like entt::registry::destroy: move the entities to the end of the living
    // ones, so that they form a range of the entity storage. Every storage
    // then removes a range in one go instead of being asked entity by entity.
    // Unlike EnTT, entities that do not exist (any more) and ones that are
    // listed twice are skipped.
    const size_t alive = entities.free_list();
    size_t count = 0;
    for (It it = first; it != last; ++it) {
      const auto e = static_cast<entt::entity>(*it);
      if (!m_reg.valid(e)) {
        continue;
      }
      const size_t target = alive - 1 - count;
      const size_t index = entities.index(e);
      if (index > target) {
        // Listed before and moved already.
        continue;
      }
      if (index != target) {
        entities.swap_elements(entities.data()[target], e);
      }
      ++count;
    }

    const auto from = entities.cend() - static_cast<std::ptrdiff_t>(alive);
    const auto to = from + static_cast<std::ptrdiff_t>(count);

    // Not through m_reg.destroy: that walks the pool table, which is only
    // touched under the context lock.
    for (internal::ComponentAccessBase *access: m_allAccesses) {
      access->storageBase->remove(from, to);
    }
    entities.erase(from, to);
  }

  template<typename... Components>
  decltype(auto) Registry::get(entt::entity e) {
    if constexpr (sizeof...(Components) == 1) {
      return (storage<Components>().get(e), ...);
    } else {
      return std::forward_as_tuple(storage<Components>().get(e)...);
    }
  }

  template<typename... Owned, typename... Get, typename... Exclude>
  auto Registry::group(entt::get_t<Get...> get, entt::exclude_t<Exclude...> exclude) {
    using GroupType = decltype(m_reg.group<Owned...>(get, exclude));

    if (void *found = m_groups[internal::groupIndex<GroupType>()].load(std::memory_order_acquire)) [[likely]] {
      return static_cast<GroupAccess<GroupType> *>(found)->group;
    }
    return createGroup<Owned...>(get, exclude)->group;
  }

  template<typename... Owned, typename... Get, typename... Exclude>
  auto *Registry::createGroup(entt::get_t<Get...> get, entt::exclude_t<Exclude...> exclude) {
    using GroupType = decltype(m_reg.group<Owned...>(get, exclude));
    constexpr size_t COUNT = sizeof...(Owned) + sizeof...(Get) + sizeof...(Exclude);

    // Before the lock is taken: creating an access object takes it as well.
    const std::array<internal::ComponentAccessBase *, COUNT> members{
        getComponentAccess<Owned>()..., getComponentAccess<Get>()..., getComponentAccess<Exclude>()...};
    std::atomic<void *> &slot = m_groups[internal::groupIndex<GroupType>()];

    std::scoped_lock lock(m_contextMutex);
    if (void *existing = slot.load(std::memory_order_relaxed)) {
      return static_cast<GroupAccess<GroupType> *>(existing);
    }

    // Creates the group in m_reg: connects it to the storages and brings
    // their current content into the order of the group.
    auto owner = std::make_unique<GroupAccess<GroupType>>(m_reg.group<Owned...>(get, exclude));
    auto *access = owner.get();
    m_reg.ctx().emplace<std::unique_ptr<GroupAccess<GroupType>>>(std::move(owner));

    // From now on a change of one component touches the others as well, so
    // they all become one family. That takes along the components of earlier
    // groups that overlap with this one.
    size_t family = members[0]->family;
    for (const internal::ComponentAccessBase *member: members) {
      family = std::min(family, member->family);
    }
    for (const internal::ComponentAccessBase *member: members) {
      const size_t merged = member->family;
      if (merged == family) {
        continue;
      }
      for (internal::ComponentAccessBase *component: m_allAccesses) {
        if (component->family == merged) {
          component->family = family;
        }
      }
    }

    m_familyVersion.fetch_add(1, std::memory_order_release);
    slot.store(access, std::memory_order_release);
    return access;
  }

  template<typename Component>
  auto Registry::getComponentAccess() -> ComponentAccess<std::remove_const_t<Component>> * {
    using Type = std::remove_const_t<Component>;

    if (internal::ComponentAccessBase *access =
            m_access[internal::componentIndex<Type>()].load(std::memory_order_acquire)) [[likely]] {
      return static_cast<ComponentAccess<Type> *>(access);
    }
    return findOrCreateComponentAccess<Type>();
  }

  template<typename Component>
  auto Registry::findOrCreateComponentAccess() -> ComponentAccess<Component> * {
    static_assert(std::is_move_constructible_v<Component>, "Component must be moveable");
    static_assert(std::is_nothrow_move_constructible_v<Component>, "Move must be no-throw for EnTT performance");
    static_assert(std::is_trivially_destructible_v<Component>);

    using AccessType = ComponentAccess<Component>;
    std::atomic<internal::ComponentAccessBase *> &slot = m_access[internal::componentIndex<Component>()];

    std::scoped_lock lock(m_contextMutex);
    if (internal::ComponentAccessBase *access = slot.load(std::memory_order_relaxed)) {
      return static_cast<AccessType *>(access);
    }

    // The only place that touches the pool table of m_reg, serialized by the
    // context lock. EnTT keeps a storage at a stable address.
    auto &storage = m_reg.storage<Component>();
    if constexpr (hasResource<Component>) {
      storage.on_destroy().template connect<&Registry::cleanupResources<Component>>(storage);
    }

    auto uptr = std::make_unique<AccessType>();
    auto *ptr = uptr.get();
    ptr->storage = &storage;
    ptr->storageBase = &storage;
    ptr->family = internal::componentIndex<Component>();
    if constexpr (hasResource<Component>) {
      ptr->heap = mi_heap_new();
    }
    // m_reg owns the access object; the slot makes it visible to all threads.
    m_reg.ctx().emplace<std::unique_ptr<AccessType>>(std::move(uptr));
    m_allAccesses.push_back(ptr);
    slot.store(ptr, std::memory_order_release);

    return ptr;
  }

  template<typename Component>
  void Registry::cleanupResources(StorageFor<Component> &storage, entt::registry &, entt::entity e) {
    auto &comp = storage.get(e);
    pfr::for_each_field(comp, [](auto &field) {
      using FieldType = std::remove_cvref_t<decltype(field)>;
      if constexpr (IsResource<FieldType>) {
        field.clear();
      }
    });
  }

  template<typename T, typename... Args>
  decltype(auto) Entity::add(Args &&...args) {
    return registry->emplace<T>(entity, std::forward<Args>(args)...);
  }

  template<typename T>
  decltype(auto) sc::ecs::Entity::get() const {
    return registry->get<T>(entity);
  }
} // namespace sc::ecs
