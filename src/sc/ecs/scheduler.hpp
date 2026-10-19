#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "sc/threading/threading.hpp"

#include "entity.hpp"
#include "world.hpp"

namespace sc::ecs::internal {

  // Memory for the components a thread wants added later, and the functions
  // it wants to modify components with. What is made in it
  // stays where it is until clear, which keeps the memory for the next time.
  class ValueStore {
  public:
    [[nodiscard]] void *allocate(size_t bytes, size_t alignment) {
      if (!m_chunks.empty()) {
        if (void *const found = place(bytes, alignment)) {
          return found;
        }
      }
      // Large enough wherever the chunk starts.
      const size_t size = std::max(CHUNK_BYTES, bytes + alignment);
      m_chunks.push_back({.memory = std::make_unique_for_overwrite<std::byte[]>(size), .size = size});
      m_used = 0;
      return place(bytes, alignment);
    }

    void clear() noexcept {
      if (m_chunks.size() > 1) {
        // One is enough to keep, and the last one is never the smallest.
        m_chunks.front() = std::move(m_chunks.back());
        m_chunks.resize(1);
      }
      m_used = 0;
    }

  private:
    static constexpr size_t CHUNK_BYTES = size_t{1} << 16;

    struct Chunk {
      std::unique_ptr<std::byte[]> memory;
      size_t size;
    };

    // In the last chunk, null if it has no room for it.
    [[nodiscard]] void *place(size_t bytes, size_t alignment) noexcept {
      const Chunk &chunk = m_chunks.back();
      void *address = chunk.memory.get() + m_used;
      size_t space = chunk.size - m_used;
      if (std::align(alignment, bytes, address, space) == nullptr) {
        return nullptr;
      }
      m_used = chunk.size - space + bytes;
      return address;
    }

    std::vector<Chunk> m_chunks;
    // Of the last chunk.
    size_t m_used = 0;
  };

  // Gives the index a component made from the recorded value unless it has
  // one, or only drops the value. The value is gone afterwards either way,
  // also if making the component throws. Null for a component without
  // members.
  template<typename T>
  void finishEmplace(ColumnBase &column, uint32_t index, void *value, bool give) {
    auto &typed = static_cast<Column<T> &>(column);
    if constexpr (Column<T>::HAS_VALUE) {
      const auto drop = [](T *made) { std::destroy_at(made); };
      const std::unique_ptr<T, decltype(drop)> made{static_cast<T *>(value), drop};
      if (give) {
        typed.tryEmplace(index, std::move(*made));
      }
    } else if (give) {
      typed.tryEmplace(index);
    }
  }

  // Calls the recorded function with the component at the index if there is
  // one, or only drops the function. Nothing runs next to it, see
  // CommandList::apply, so without the making bit. The function is gone
  // afterwards either way, also if it throws.
  template<typename T, typename Function>
  void finishModify(ColumnBase &column, uint32_t index, void *value, bool give) {
    auto &typed = static_cast<Column<T> &>(column);
    const auto drop = [](Function *function) { std::destroy_at(function); };
    const std::unique_ptr<Function, decltype(drop)> function{static_cast<Function *>(value), drop};
    if (give && typed.contains(index)) {
      (*function)(typed.get(index));
    }
  }

  // What one thread wants done once its stage is over. On cache lines of its
  // own, the threads next to it write theirs at the same time.
  class alignas(64) CommandList {
  public:
    CommandList() = default;
    ~CommandList() { clear(); }

    CommandList(CommandList &&) = delete;
    CommandList &operator=(CommandList &&) = delete;

    void erase(Entity entity, ColumnBase &column) {
      m_changes.push_back({.entity = entity, .column = &column, .finish = nullptr, .value = nullptr});
      note(entity);
    }

    template<typename T, typename... Args>
    void emplace(Entity entity, Column<T> &column, Args &&...args) {
      // First, so that a value that was made is always one of a change.
      m_changes.push_back({.entity = entity, .column = &column, .finish = &finishEmplace<T>, .value = nullptr});
      if constexpr (Column<T>::HAS_VALUE) {
        try {
          m_changes.back().value = ::new (m_values.allocate(sizeof(T), alignof(T))) T{std::forward<Args>(args)...};
        } catch (...) {
          m_changes.pop_back();
          throw;
        }
      }
      note(entity);
    }

    template<typename T, typename Function>
    void modify(Entity entity, Column<T> &column, Function &&function) {
      using Stored = std::decay_t<Function>;
      // First, as in emplace.
      m_changes.push_back({.entity = entity, .column = &column, .finish = &finishModify<T, Stored>, .value = nullptr});
      try {
        m_changes.back().value =
            ::new (m_values.allocate(sizeof(Stored), alignof(Stored))) Stored(std::forward<Function>(function));
      } catch (...) {
        m_changes.pop_back();
        throw;
      }
      note(entity);
    }

    void destroy(Entity entity) { m_destroyed.push_back(entity); }

    // Taken by the threads outside the pool around every change they record:
    // they all share one list, see Commands::withList.
    [[nodiscard]] std::mutex &mutex() noexcept { return m_mutex; }

    // A component to add, remove or modify: the entity and the column of the
    // component.
    struct Change {
      Entity entity;
      ColumnBase *column;
      // finishEmplace of a component to add or finishModify of one to
      // modify, which also drop the value; null for one to remove, and once
      // the change is done.
      void (*finish)(ColumnBase &column, uint32_t index, void *value, bool give);
      // The component to add, or the function of modify.
      void *value;
    };

    // What was recorded, for CommandApplier.
    [[nodiscard]] std::span<Change> changes() noexcept { return m_changes; }
    // The page of entities all changes are in, or NO_PAGE if they are in
    // several. Meaningless without changes.
    [[nodiscard]] uint32_t page() const noexcept { return m_page; }
    [[nodiscard]] std::span<const Entity> destroyed() const noexcept { return m_destroyed; }

    // Forgets what was recorded once all of it is done: no value is left to
    // drop, so without going through the changes again (1M of them are 32 MB).
    void clearDone() noexcept {
      m_changes.clear();
      m_values.clear();
      m_destroyed.clear();
    }

    // Drops what was recorded: the values of the changes not done.
    void clear() noexcept {
      for (const Change &change: m_changes) {
        if (change.finish != nullptr) {
          change.finish(*change.column, indexOf(change.entity), change.value, false);
        }
      }
      m_changes.clear();
      m_values.clear();
      m_destroyed.clear();
    }

    static constexpr uint32_t NO_PAGE = ~uint32_t{0};

  private:
    // Keeps page up to date. One comparison per change, which spares
    // CommandApplier counting 1M changes (about 0.6 ms on all threads) to
    // find that they all hit one page.
    void note(Entity entity) noexcept {
      const uint32_t page = indexOf(entity) >> PAGE_BITS;
      if (m_changes.size() == 1) {
        m_page = page;
      } else if (page != m_page) {
        m_page = NO_PAGE;
      }
    }

    std::vector<Change> m_changes;
    uint32_t m_page = NO_PAGE;
    ValueStore m_values;
    // The entities to destroy.
    std::vector<Entity> m_destroyed;
    std::mutex m_mutex;
  };

  // Does what the lists of a stage recorded, once nothing runs any more: the
  // changes of the components first, then the entities to destroy.
  //
  // The changes are sorted by the page of their entity with PageSort,
  // keeping their order (the lists one after the other, each in the order it
  // was recorded), and every page goes to one thread. So the changes of an entity are done in
  // their order, and no other thread touches the page in any column, which
  // lets the erases of a page be done a word of the mask at a time
  // (Column::eraseAlone of a KillPage). The entities are destroyed at once,
  // with World::parallelDestroy.
  class CommandApplier {
  public:
    // Does what the lists recorded and clears them. If a change throws, the
    // pages that are not started yet and the entities to destroy are left in
    // the lists, for clear to drop, and the first exception is rethrown;
    // pages that other threads are working on are finished.
    void apply(World &world, std::span<CommandList *const> lists) {
      size_t total = 0;
      for (CommandList *const list: lists) {
        total += list->changes().size();
      }
      if (onePage(lists)) {
        applyInOrder(world, lists);
      } else {
        // A thread for every so many changes, as World::parallelEach does for
        // entities.
        applySpread(world, lists, total / CHANGES_PER_THREAD);
      }

      m_destroyed.clear();
      for (CommandList *const list: lists) {
        m_destroyed.insert(m_destroyed.end(), list->destroyed().begin(), list->destroyed().end());
      }
      if (!m_destroyed.empty()) {
        world.parallelDestroy(std::span<const Entity>{m_destroyed});
      }
      // All of it done, also the changes of the null entity, which only drop
      // their values.
      for (CommandList *const list: lists) {
        list->clearDone();
      }
    }

  private:
    using Change = CommandList::Change;

    // A page is not worth waking a thread for below this.
    static constexpr size_t CHANGES_PER_THREAD = 4096;

    // The erases of a page in one column, gathered until something else comes.
    struct Kill {
      ColumnBase *column;
      KillPage page;
    };

    // Whether all changes of all lists are in one page, or there are none:
    // nothing to spread then, as the lists noted while recording.
    [[nodiscard]] static bool onePage(std::span<CommandList *const> lists) noexcept {
      uint32_t page = CommandList::NO_PAGE;
      for (CommandList *const list: lists) {
        if (list->changes().empty()) {
          continue;
        }
        if (list->page() == CommandList::NO_PAGE || (page != CommandList::NO_PAGE && list->page() != page)) {
          return false;
        }
        page = list->page();
      }
      return true;
    }

    // Every change on the calling thread, in the order of the lists.
    static void applyInOrder(World &world, std::span<CommandList *const> lists) {
      std::vector<Kill> kills;
      for (CommandList *const list: lists) {
        for (Change &change: list->changes()) {
          applyChange(world, change, kills);
        }
      }
      flush(kills);
    }

    // Every page on one thread. If a change throws, the pages that are not
    // started yet are left, and the first exception is rethrown once the
    // pages that other threads are working on are finished.
    void applySpread(World &world, std::span<CommandList *const> lists, size_t helpers) {
      m_sort.clear();
      for (CommandList *const list: lists) {
        m_sort.add(list->changes());
      }
      const auto index = [](const Change &change) { return indexOf(change.entity); };
      // A change of the null entity is done right away, which only drops its
      // value.
      m_sort.count(helpers, index, [](Change &change) {
        if (change.finish != nullptr) {
          const auto finish = change.finish;
          change.finish = nullptr;
          finish(*change.column, indexOf(change.entity), change.value, false);
        }
      });
      if (m_sort.pages().size() <= 1) {
        // Only one after all, past those of the null entity. The counts go
        // back to zero first, whatever happens then.
        m_sort.uncount();
        applyInOrder(world, lists);
        return;
      }
      m_sort.place(helpers, index);
      std::atomic_flag failed;
      std::exception_ptr error;
      threading::parallelFor(m_sort.pages().size(), 1, helpers, [&](size_t first, size_t last) {
        std::vector<Kill> kills;
        for (size_t i = first; i < last; ++i) {
          if (failed.test(std::memory_order_relaxed)) {
            return;
          }
          try {
            applyPage(world, m_sort.page(i), kills);
          } catch (...) {
            if (!failed.test_and_set(std::memory_order_acq_rel)) {
              error = std::current_exception();
            }
            return;
          }
        }
      });
      if (error) {
        std::rethrow_exception(error);
      }
    }

    // The changes of one page, in their order. The erases that come one
    // after the other are gathered per column and done together before the
    // next change of another kind.
    static void applyPage(World &world, std::span<Change *const> changes, std::vector<Kill> &kills) {
      for (Change *const change: changes) {
        applyChange(world, *change, kills);
      }
      flush(kills);
    }

    static void applyChange(World &world, Change &change, std::vector<Kill> &kills) {
      const uint32_t index = indexOf(change.entity);
      // Not that of an entity that got the index since.
      const bool valid = world.valid(change.entity);
      if (change.finish == nullptr) {
        if (valid) {
          kill(kills, *change.column, index);
        }
        return;
      }
      flush(kills);
      // Done before the call: finish drops the value whatever happens.
      const auto finish = change.finish;
      change.finish = nullptr;
      finish(*change.column, index, change.value, valid);
    }

    static void kill(std::vector<Kill> &kills, ColumnBase &column, uint32_t index) {
      auto found = std::ranges::find(kills, &column, &Kill::column);
      if (found == kills.end()) {
        kills.push_back({.column = &column, .page = {}});
        found = kills.end() - 1;
      }
      KillPage &page = found->page;
      page.number = index >> PAGE_BITS;
      const uint32_t slot = index & (PAGE_SIZE - 1);
      page.words[slot / 64] |= uint64_t{1} << (slot % 64);
      page.touched |= uint64_t{1} << (slot / 64);
    }

    // Does the gathered erases and makes the pages empty again for the next.
    static void flush(std::vector<Kill> &kills) noexcept {
      for (Kill &kill: kills) {
        if (kill.page.touched == 0) {
          continue;
        }
        kill.column->eraseAlone(std::span<const KillPage>{&kill.page, 1});
        for (uint64_t touched = kill.page.touched; touched != 0; touched &= touched - 1) {
          kill.page.words[static_cast<size_t>(std::countr_zero(touched))] = 0;
        }
        kill.page.touched = 0;
      }
    }

    PageSort<Change> m_sort;
    // Of all lists.
    std::vector<Entity> m_destroyed;
  };

  // Defined below, with the parameters of systems.
  template<typename F>
  struct SystemSignature;

  // The component a function for Modify takes: its only parameter.
  template<typename Params>
  struct OnlyParam {
    static_assert(sizeof(Params) == 0, "the function takes the component, and nothing else");
  };
  template<typename Param>
  struct OnlyParam<List<Param>> {
    using Type = std::remove_cvref_t<Param>;
  };

  template<typename F>
  using ModifiedBy = typename OnlyParam<typename SystemSignature<F>::Params>::Type;

} // namespace sc::ecs::internal

namespace sc::ecs {

  // The entities that have the given components, as a parameter of a system:
  //
  //   void move(Query<Position, const Velocity> query) {
  //     query.each([](Position &pos, const Velocity &vel) { ... });
  //   }
  //
  // A const component is only read, Exclude names components the entities must
  // not have. This is what tells the scheduler which systems may run at the
  // same time.
  template<typename... Terms>
  class Query {
    template<typename Component>
    static constexpr bool named = (std::same_as<Component, Terms> || ...);

  public:
    explicit Query(World &world) : m_world(&world), m_columns(columnsOf(world, typename Lists::GetList{})) {}

    // See World::each.
    template<typename Function>
    void each(Function &&function) const {
      m_world->template each<Terms...>(function);
    }

    // See World::parallelEach.
    template<typename Function>
    void parallelEach(Function &&function, size_t grain = 1) const {
      m_world->template parallelEach<Terms...>(function, grain);
    }

    // The component of any entity that exists and has it, named the way the
    // query names it. In the function of a parallel loop only a const one:
    // the scheduler keeps other systems from writing it, but not the other
    // threads of the same loop.
    template<typename Component>
      requires named<Component>
    [[nodiscard]] Component &get(Entity entity) const {
      assert(m_world->valid(entity));
      assert(internal::mayGet<Component>() &&
             "sc::ecs: get for writing in a parallel loop, where another thread of it may write the same component");
      return std::get<World::ColumnOf<Component> *>(m_columns)->get(indexOf(entity));
    }

    template<typename Component>
      requires named<Component> || named<const Component>
    [[nodiscard]] bool contains(Entity entity) const {
      return m_world->template contains<Component>(entity);
    }

  private:
    using Lists = internal::Split<internal::List<>, internal::List<>, Terms...>;

    template<typename... Get>
    static auto columnsOf(World &world, internal::List<Get...>) {
      return std::tuple<World::ColumnOf<Get> *...>{&world.column<Get>()...};
    }

    World *m_world;
    // Looked up once, so that get does not have to.
    decltype(columnsOf(std::declval<World &>(), typename Lists::GetList{})) m_columns;
  };

  // Lets a system add and remove the components it names, right away:
  //
  //   scheduler.addParallel([](Entity entity, Change<Frozen> change, const Health &health) {
  //     if (health.value < 10.0f) {
  //       change.emplace<Frozen>(entity);
  //     }
  //   });
  //
  // To the scheduler this is writing the components, so no other system uses
  // them at the same time. Within the system:
  //  - a loop must not change the components it goes through;
  //  - the function of a parallel loop uses emplace only for the entity it is
  //    called with and for entities it made with create, which no other
  //    thread knows of. Any other entity it gives a component with
  //    tryEmplace, as several threads may be doing at the same time, and
  //    takes it away with Commands::erase: erase would take it away from
  //    under a thread that is using it.
  template<typename... Components>
  class Change {
    template<typename Component>
    static constexpr bool named = (std::same_as<Component, Components> || ...);

  public:
    explicit Change(World &world) : m_world(&world) {}

    // An entity to give the named components to. Safe to call from the
    // function of a parallel loop, like World::create.
    [[nodiscard]] Entity create() const { return m_world->create(); }

    // The same in the area of a set of components, see World::create: an
    // entity a system spawns lies with the others of its kind.
    template<typename... Set>
      requires(sizeof...(Set) != 0)
    [[nodiscard]] Entity create() const {
      return m_world->template create<Set...>();
    }

    // An entity with named components, right away, in the area of their set.
    template<typename First, typename... Rest>
      requires(named<First> && (named<Rest> && ...))
    [[nodiscard]] Entity create(const First &first, const Rest &...rest) const {
      return m_world->create(first, rest...);
    }

    template<typename Component, typename... Args>
      requires named<Component>
    Emplaced emplace(Entity entity, Args &&...args) const { // NOLINT(modernize-use-nodiscard)
      return m_world->template emplace<Component>(entity, std::forward<Args>(args)...);
    }

    // See World::tryEmplace. Not [[nodiscard]], as World::tryEmplace and
    // World::erase are not: whether this thread was first is often of no
    // interest.
    template<typename Component, typename... Args>
      requires named<Component>
    Emplaced tryEmplace(Entity entity, Args &&...args) const { // NOLINT(modernize-use-nodiscard)
      return m_world->template tryEmplace<Component>(entity, std::forward<Args>(args)...);
    }

    template<typename Component>
      requires named<Component>
    bool erase(Entity entity) const { // NOLINT(modernize-use-nodiscard)
      return m_world->template erase<Component>(entity);
    }

    template<typename Component>
      requires named<Component>
    [[nodiscard]] bool contains(Entity entity) const {
      return m_world->template contains<Component>(entity);
    }

  private:
    World *m_world;
  };

  // Lets a system change the values of the components it names, on any
  // entity, right away and from several threads for the same one:
  //
  //   scheduler.addParallel([](Modify<Health> modify, const Target &target) {
  //     modify(target.entity, [](Health &health) { health.value -= 10; });
  //   });
  //
  // To the scheduler this is writing the components, so no other system uses
  // them at the same time. Within the system modify locks a component and
  // nothing else does, so nothing else may use them: the function of a
  // parallel loop does not take them as parameters, and no query of the
  // system names them, not even for reading. Both are checked when the
  // system is added. To read such a component, read it in the function of
  // modify.
  //
  // Modify or Commands::modify: where few threads hit the same entity,
  // Modify, which runs right away where the threads are; where many hit few,
  // Commands::modify, which does not touch the entity before the stage is
  // over. Every modify of a thread reaches for the cache line of the lock bit,
  // and every one that takes it writes it twice; with many threads on one
  // entity the line goes from core to core for each of them, whether the bit
  // is free or not. ecs_compare, 1M entities damaging another one, all
  // threads, ns per hit, Modify against Commands::modify: random target 1.4
  // and 3.6, one of 1 000 targets 14.6 and 3.2, the same target 15.6 and 3.4.
  template<typename... Components>
  class Modify {
    static_assert((!std::is_const_v<Components> && ...), "Modify is for writing; a const component is read with Query");

    template<typename Component>
    static constexpr bool named = (std::same_as<Component, Components> || ...);

  public:
    explicit Modify(World &world) : m_world(&world) {}

    // See World::modify. The component is the one the function takes, by
    // reference.
    template<typename Function>
    bool operator()(Entity entity, Function &&function) const { // NOLINT(modernize-use-nodiscard)
      using Component = internal::ModifiedBy<std::decay_t<Function>>;
      static_assert(named<Component>, "the function takes a component the Modify names");
      return m_world->template modify<Component>(entity, std::forward<Function>(function));
    }

  private:
    World *m_world;
  };

  // Records what a system cannot do right away: destroying entities, which
  // touches every component, and removing a component another thread may be
  // using. Adding a component is recorded for a system that would otherwise
  // have to write it with Change, and keep the systems that use it waiting;
  // changing its value, for one that would otherwise use Modify, for the same
  // reason and to need no lock.
  // It is done once all systems that run at the same time are done, so
  // nothing an entity has goes away or turns up while they run. Safe to use
  // from the function of a parallel loop, for any entity and from several
  // threads for the same one, also from threads outside the pool, which take
  // turns with a list they share.
  //
  // What one thread records for the components is done in that order, so a
  // component that is erased and then emplaced is replaced. The threads come
  // one after the other, in no particular order, and the entities are
  // destroyed after the components of all threads, all at once. The changes
  // are done on several threads, every page of entities on one (see
  // CommandApplier).
  class Commands {
  public:
    Commands(World &world, std::span<internal::CommandList> lists) : m_world(&world), m_lists(lists) {}

    // An entity to give components to with emplace. It exists right away,
    // without components, so no loop comes across it before the stage is
    // over. Safe to call from the function of a parallel loop, like
    // World::create.
    [[nodiscard]] Entity create() const { return m_world->create(); }

    // The same in the area of a set of components, see World::create: an
    // entity a system spawns lies with the others of its kind once the
    // recorded components are there.
    template<typename... Set>
      requires(sizeof...(Set) != 0)
    [[nodiscard]] Entity create() const {
      return m_world->template create<Set...>();
    }

    // Does nothing if the entity does not exist any more by then.
    void destroy(Entity entity) const {
      withList([&](internal::CommandList &list) { list.destroy(entity); });
    }

    // Makes the component now, from the arguments, and gives it to the entity
    // later. Does nothing if the entity does not exist any more by then or
    // has the component already: the one that was made is dropped. Like
    // erase, this does not count as writing the component.
    template<typename Component, typename... Args>
    void emplace(Entity entity, Args &&...args) const {
      auto &column = m_world->template column<Component>();
      withList([&](internal::CommandList &list) { list.emplace(entity, column, std::forward<Args>(args)...); });
    }

    // Does nothing if the entity does not exist any more by then or does not
    // have the component. A system that does this does not count as writing
    // the component: the systems that use it run at the same time and still
    // find it.
    template<typename Component>
    void erase(Entity entity) const {
      auto &column = m_world->template column<Component>();
      withList([&](internal::CommandList &list) { list.erase(entity, column); });
    }

    // Calls the function with the component of the entity later, as
    // Modify does now:
    //
    //   commands.modify(target, [](Health &health) { health.value -= 10; });
    //
    // The component is the one the function takes. The function is moved
    // into the list now and called once the stage is over, with nothing else
    // running, so without a lock; what it refers to must still be there then.
    // Nothing happens if the entity does not exist any more by then or does
    // not have the component. Like erase, this does not count as writing the
    // component: the systems that use it run at the same time and see it as
    // it was.
    //
    // Of two threads that record a change for the same component, both
    // happen, one after the other: in the order of the threads, which entities
    // a thread gets in a parallel loop is not fixed, so the order is only the
    // same from run to run for one thread. The result is the same where the
    // order does not matter, as with damage that is subtracted.
    //
    // Faster than Modify where many threads hit few entities, slower where
    // they hit many, see Modify.
    template<typename Function>
    void modify(Entity entity, Function &&function) const {
      using Component = internal::ModifiedBy<std::decay_t<Function>>;
      static_assert(std::is_invocable_v<std::decay_t<Function> &, Component &>,
                    "the function takes the component by reference");
      static_assert(internal::Column<Component>::HAS_VALUE, "a component without members has no value to modify");
      auto &column = m_world->template column<Component>();
      withList([&](internal::CommandList &list) { list.modify(entity, column, std::forward<Function>(function)); });
    }

  private:
    // Calls the function with the list of the calling thread. The last list
    // is shared by the threads outside the pool, which lock it for the call.
    template<typename Function>
    void withList(Function &&function) const {
      const uint32_t thread = threading::threadId();
      if (thread < m_lists.size() - 1) [[likely]] {
        function(m_lists[thread]);
        return;
      }
      internal::CommandList &shared = m_lists.back();
      const std::scoped_lock lock{shared.mutex()};
      function(shared);
    }

    World *m_world;
    // One for every sc::threading::threadId, and one more for the threads
    // outside the pool.
    std::span<internal::CommandList> m_lists;
  };

} // namespace sc::ecs

namespace sc::ecs::internal {

  // What a system reads and writes, collected from its parameters.
  struct SystemAccess {
    struct Component {
      size_t index;
      bool write;
    };

    std::vector<Component> components;
    // The system takes the world itself and may do anything with it.
    bool exclusive = false;
    bool commands = false;

    // Several parameters may name the same component; writing wins.
    void add(size_t index, bool write) {
      for (Component &component: components) {
        if (component.index == index) {
          component.write = component.write || write;
          return;
        }
      }
      components.push_back({.index = index, .write = write});
    }

    // Whether the two must not run at the same time.
    [[nodiscard]] bool conflictsWith(const SystemAccess &other) const noexcept {
      if (exclusive || other.exclusive) {
        return true;
      }
      for (const Component &mine: components) {
        for (const Component &theirs: other.components) {
          if (mine.index == theirs.index && (mine.write || theirs.write)) {
            return true;
          }
        }
      }
      return false;
    }
  };

  // What the parameters of a system are made from when it runs.
  struct SystemContext {
    World &world;
    std::span<CommandList> commands;
  };

  // How one kind of system parameter is declared to the scheduler and how its
  // value is made when the system runs.
  template<typename Param>
  struct SystemParam;

  template<typename... Terms>
  struct SystemParam<Query<Terms...>> {
    static void declare(SystemAccess &access) {
      using Lists = Split<List<>, List<>, Terms...>;
      declare(access, typename Lists::GetList{}, typename Lists::ExcludedList{});
    }

    static Query<Terms...> fetch(const SystemContext &context) { return Query<Terms...>{context.world}; }

  private:
    template<typename... Get, typename... Excluded>
    static void declare(SystemAccess &access, List<Get...>, List<Excluded...>) {
      (access.add(componentIndex<std::remove_const_t<Get>>(), !std::is_const_v<Get>), ...);
      // An excluded component is only looked up, never changed.
      (access.add(componentIndex<std::remove_const_t<Excluded>>(), false), ...);
    }
  };

  template<typename... Components>
  struct SystemParam<Change<Components...>> {
    static void declare(SystemAccess &access) { (access.add(componentIndex<Components>(), true), ...); }

    static Change<Components...> fetch(const SystemContext &context) { return Change<Components...>{context.world}; }
  };

  template<typename... Components>
  struct SystemParam<Modify<Components...>> {
    static void declare(SystemAccess &access) { (access.add(componentIndex<Components>(), true), ...); }

    static Modify<Components...> fetch(const SystemContext &context) { return Modify<Components...>{context.world}; }
  };

  template<>
  struct SystemParam<Commands> {
    static void declare(SystemAccess &access) { access.commands = true; }

    static Commands fetch(const SystemContext &context) { return Commands{context.world, context.commands}; }
  };

  template<>
  struct SystemParam<World &> {
    static void declare(SystemAccess &access) { access.exclusive = true; }

    static World &fetch(const SystemContext &context) { return context.world; }
  };

  // Queries, Change and Commands are taken by value or by const reference, the
  // world only by reference.
  template<typename Param>
  using SystemParamFor =
      SystemParam<std::conditional_t<std::is_same_v<Param, World &>, World &, std::remove_cvref_t<Param>>>;

  // The parameter types of a system: a function pointer or an object with a
  // single, non-template operator().
  template<typename F>
  struct SystemSignature : SystemSignature<decltype(&F::operator())> {};

  template<typename R, typename... Args>
  struct SystemSignature<R (*)(Args...)> {
    using Params = List<Args...>;
  };
  template<typename R, typename... Args>
  struct SystemSignature<R (*)(Args...) noexcept> : SystemSignature<R (*)(Args...)> {};
  template<typename C, typename R, typename... Args>
  struct SystemSignature<R (C::*)(Args...)> : SystemSignature<R (*)(Args...)> {};
  template<typename C, typename R, typename... Args>
  struct SystemSignature<R (C::*)(Args...) const> : SystemSignature<R (*)(Args...)> {};
  template<typename C, typename R, typename... Args>
  struct SystemSignature<R (C::*)(Args...) noexcept> : SystemSignature<R (*)(Args...)> {};
  template<typename C, typename R, typename... Args>
  struct SystemSignature<R (C::*)(Args...) const noexcept> : SystemSignature<R (*)(Args...)> {};

  // A parameter of a loop function that is neither the entity nor a component.
  template<typename Param>
  inline constexpr bool isLoopExtra = false;
  template<typename... Components>
  inline constexpr bool isLoopExtra<Change<Components...>> = true;
  template<typename... Components>
  inline constexpr bool isLoopExtra<Modify<Components...>> = true;
  template<>
  inline constexpr bool isLoopExtra<Commands> = true;
  template<typename... Terms>
  inline constexpr bool isLoopExtra<Query<Terms...>> = true;

  // The parameters of a function for Scheduler::addParallel: the entity, then
  // Change, Commands and queries, all of them optional, and then the
  // components.
  template<bool WithEntity, typename Extras, typename... Rest>
  struct LoopRest;

  template<bool WithEntity, typename... Extras, typename... Components>
  struct LoopRest<WithEntity, List<Extras...>, Components...> {
    static constexpr bool ENTITY = WithEntity;
    using ExtraList = List<Extras...>;
    using ComponentList = List<Components...>;
  };

  template<bool WithEntity, typename... Extras, typename First, typename... Rest>
    requires isLoopExtra<std::remove_cvref_t<First>>
  struct LoopRest<WithEntity, List<Extras...>, First, Rest...>
      : LoopRest<WithEntity, List<Extras..., std::remove_cvref_t<First>>, Rest...> {};

  template<typename Params>
  struct LoopSignature;
  template<typename... Params>
  struct LoopSignature<List<Params...>> : LoopRest<false, List<>, Params...> {};
  template<typename... Params>
  struct LoopSignature<List<Entity, Params...>> : LoopRest<true, List<>, Params...> {};

  // The term of a query for a component a loop function takes: only a
  // reference to non-const lets the function write it.
  template<typename Param>
  using TermOf =
      std::conditional_t<std::is_lvalue_reference_v<Param> && !std::is_const_v<std::remove_reference_t<Param>>,
                         std::remove_reference_t<Param>, const std::remove_cvref_t<Param>>;

  template<typename... Lists>
  struct Concat {
    using Type = List<>;
  };
  template<typename... Items>
  struct Concat<List<Items...>> {
    using Type = List<Items...>;
  };
  template<typename... First, typename... Second, typename... Rest>
  struct Concat<List<First...>, List<Second...>, Rest...> : Concat<List<First..., Second...>, Rest...> {};

  // The components a parameter of a system names for modify.
  template<typename Param>
  struct ModifiedOf {
    using Type = List<>;
  };
  template<typename... Components>
  struct ModifiedOf<Modify<Components...>> {
    using Type = List<Components...>;
  };

  // The components a parameter of a system reads or writes without modify:
  // those a query gets. Excluded ones are only looked up in the mask, which
  // modify does not change.
  template<typename Param>
  struct GottenOf {
    using Type = List<>;
  };
  template<typename... Terms>
  struct GottenOf<Query<Terms...>> {
    using Type = typename Split<List<>, List<>, Terms...>::GetList;
  };

  template<typename Component, typename... Gotten>
  inline constexpr bool isGotten = (std::same_as<Component, std::remove_const_t<Gotten>> || ...);

  template<typename Modified, typename Gotten>
  inline constexpr bool overlaps = false;
  template<typename... Modified, typename... Gotten>
  inline constexpr bool overlaps<List<Modified...>, List<Gotten...>> = (isGotten<Modified, Gotten...> || ...);

  // Whether a system with these parameters uses a component it modifies
  // also past modify, which takes no lock: through a query, or as a
  // component of its loop, which comes as a query as well.
  template<typename... Params>
  inline constexpr bool usesModifiedOtherwise =
      overlaps<typename Concat<typename ModifiedOf<std::remove_cvref_t<Params>>::Type...>::Type,
               typename Concat<typename GottenOf<std::remove_cvref_t<Params>>::Type...>::Type>;

  // Calls the function for every item, for more than one item on the thread
  // pool, and returns once all calls are done. Rethrows the first exception.
  template<typename Items, typename Function>
  void runAll(const Items &items, Function &&function) {
    if (items.empty()) [[unlikely]] {
      return;
    }
    if (items.size() == 1) {
      function(items.front());
      return;
    }

    // Caught here and not by parallelFor, which would not start the calls
    // that are left: every call is made, whatever the others do.
    std::atomic_flag failed;
    std::exception_ptr error;
    threading::parallelFor(items.size(), 1, [&](size_t first, size_t last) {
      for (size_t i = first; i < last; ++i) {
        try {
          function(items[i]);
        } catch (...) {
          if (!failed.test_and_set(std::memory_order_acq_rel)) {
            error = std::current_exception();
          }
        }
      }
    });

    if (error) {
      std::rethrow_exception(error);
    }
  }

} // namespace sc::ecs::internal

namespace sc::ecs {

  // Runs systems on a world, as many of them at the same time as their
  // parameters allow.
  //
  // A system is a function whose parameters say what it uses:
  //
  //   void move(Query<Position, const Velocity> query);   // writes Position, reads Velocity
  //   void freeze(Query<const Health> query, Change<Frozen> change);   // adds and removes Frozen
  //   void hit(Query<const Target> query, Modify<Health> modify);   // changes Health of any entity, locked
  //   void reap(Query<const Lifetime> query, Commands commands);   // destroys entities, later
  //   void load(World &world);                             // may do anything, runs alone
  //
  // Two systems run at the same time if neither writes a component the other
  // one uses. Systems that cannot run together keep the order in which they
  // were added. A system without parameters says nothing about what it uses
  // and therefore runs alone, like one that takes the world.
  class Scheduler {
  public:
    template<typename F>
    Scheduler &add(F &&system, std::string name = {}) {
      using Function = std::decay_t<F>;
      return addSystem(std::forward<F>(system), std::move(name),
                       typename internal::SystemSignature<Function>::Params{});
    }

    // Adds a system that is the body of a loop: the function is called with
    // the components of every entity that has all of them, on several threads
    // at once.
    //
    //   scheduler.addParallel([](Position &pos, const Velocity &vel) { ... });
    //
    // A component taken by reference to non-const is written, any other one is
    // only read. Before the components, the function may take the entity and,
    // after it, Change, Commands, Modify and queries, the last two for
    // entities other than its own:
    //
    //   scheduler.addParallel([](Entity entity, Commands commands, const Health &health) { ... });
    //   scheduler.addParallel([](Query<const Health> others, const Target &target) { ... });
    //   scheduler.addParallel([](Modify<Health> modify, const Target &target) { ... });
    //
    // A query there only reads: the other threads of the loop may be at the
    // same entity, and get asserts in debug builds. To write, Modify.
    //
    // grain is the number of pages a thread takes at a time.
    template<typename F>
    Scheduler &addParallel(F &&function, std::string name = {}, size_t grain = 1) {
      using Signature = internal::LoopSignature<typename internal::SystemSignature<std::decay_t<F>>::Params>;
      return addLoop(std::forward<F>(function), std::move(name), grain, typename Signature::ExtraList{},
                     typename Signature::ComponentList{});
    }

    // The same with the query to go through named, for what the parameters of
    // the function cannot say: excluded components and components the entities
    // need but the function does not take.
    //
    //   scheduler.addParallel<Query<Position, const Velocity, Exclude<Frozen>>>(
    //       [](Position &pos, const Velocity &vel) { ... });
    //
    // The function takes the components in the order the query names them,
    // without the ones that have no members.
    template<typename QueryType, typename F>
    Scheduler &addParallel(F &&function, std::string name = {}, size_t grain = 1) {
      using Signature = internal::LoopSignature<typename internal::SystemSignature<std::decay_t<F>>::Params>;
      return addLoopOver(std::type_identity<QueryType>{}, std::forward<F>(function), std::move(name), grain,
                         typename Signature::ExtraList{});
    }

    // Runs every system once. Stages with more than one system and systems
    // added with addParallel run on the thread pool, so sc::threading::init
    // must have been called before, and run must not be called from a pool
    // thread.
    //
    // If a system throws, the systems running at the same time still finish,
    // what they recorded through Commands is not done, later systems are
    // skipped and the first exception is rethrown. The same goes for a
    // component of Commands::emplace that cannot be made: what the stage
    // recorded and is not done yet is dropped.
    void run(World &world) {
      const std::vector<std::vector<size_t>> &stages = this->stages();

      // Here and not when the system is added: the pool may be set up later.
      const size_t threads = threading::threadSlots();
      for (System &system: m_systems) {
        // One more for the threads outside the pool, see Commands::withList.
        if (system.access.commands && system.commands.size() < threads + 1) {
          system.commands = std::vector<internal::CommandList>(threads + 1);
        }
      }

      for (const std::vector<size_t> &stage: stages) {
        try {
          internal::runAll(stage, [this, &world](size_t index) {
            System &system = m_systems[index];
            system.run({.world = world, .commands = system.commands});
          });
        } catch (...) {
          clearCommands(stage);
          throw;
        }

        // Now that nothing is running: nobody is using what goes away.
        try {
          m_stageLists.clear();
          for (const size_t index: stage) {
            for (internal::CommandList &list: m_systems[index].commands) {
              m_stageLists.push_back(&list);
            }
          }
          m_applier.apply(world, m_stageLists);
        } catch (...) {
          clearCommands(stage);
          throw;
        }
      }
    }

    [[nodiscard]] size_t size() const noexcept { return m_systems.size(); }

    [[nodiscard]] const std::string &name(size_t system) const { return m_systems[system].name; }

    // The systems in the order they run, by index in the order they were
    // added: one stage after the other, the systems of a stage at the same
    // time.
    [[nodiscard]] const std::vector<std::vector<size_t>> &stages() {
      if (!m_stagesValid) {
        // A system runs in the stage after the last earlier one it conflicts
        // with.
        std::vector<size_t> stageOf(m_systems.size(), 0);
        size_t count = 0;
        for (size_t i = 0; i < m_systems.size(); ++i) {
          for (size_t earlier = 0; earlier < i; ++earlier) {
            if (m_systems[i].access.conflictsWith(m_systems[earlier].access)) {
              stageOf[i] = std::max(stageOf[i], stageOf[earlier] + 1);
            }
          }
          count = std::max(count, stageOf[i] + 1);
        }
        m_stages.assign(count, {});
        for (size_t i = 0; i < m_systems.size(); ++i) {
          m_stages[stageOf[i]].push_back(i);
        }
        m_stagesValid = true;
      }
      return m_stages;
    }

  private:
    struct System {
      std::move_only_function<void(const internal::SystemContext &)> run;
      std::string name;
      internal::SystemAccess access;
      // What the threads recorded through Commands, one list for every pool
      // thread and one for all others; empty for a system that does not take
      // Commands.
      std::vector<internal::CommandList> commands;
    };

    // Drops what the systems of a stage recorded through Commands.
    void clearCommands(const std::vector<size_t> &stage) noexcept {
      for (const size_t index: stage) {
        for (internal::CommandList &list: m_systems[index].commands) {
          list.clear();
        }
      }
    }

    template<typename F, typename... Params>
    Scheduler &addSystem(F &&system, const std::string &name, internal::List<Params...>) {
      static_assert(!internal::usesModifiedOtherwise<Params...>,
                    "a component the system modifies is not also taken by its loop or named by its queries: modify "
                    "locks it and they do not; read it in the function of modify");
      internal::SystemAccess access;
      (internal::SystemParamFor<Params>::declare(access), ...);
      if (sizeof...(Params) == 0) {
        access.exclusive = true;
      }

      m_systems.push_back(System{
          .run =
              [function = std::decay_t<F>(std::forward<F>(system))](const internal::SystemContext &context) mutable {
                function(internal::SystemParamFor<Params>::fetch(context)...);
              },
          .name = std::move(name),
          .access = std::move(access),
          .commands = {},
      });
      m_stagesValid = false;
      return *this;
    }

    // addParallel for the query of the components the function is called with.
    template<typename F, typename... Extras, typename... Components>
    Scheduler &addLoop(F &&function, std::string name, size_t grain, internal::List<Extras...> extras,
                       internal::List<Components...>) {
      static_assert(sizeof...(Components) != 0, "the function takes the components of an entity");
      static_assert((!std::is_rvalue_reference_v<Components> && ...), "a component is taken by value or reference");
      static_assert((!std::is_empty_v<std::remove_cvref_t<Components>> && ...),
                    "a component without members cannot be a parameter");
      return addLoopOver(std::type_identity<Query<internal::TermOf<Components>...>>{}, std::forward<F>(function),
                         std::move(name), grain, extras);
    }

    template<typename... Terms, typename F, typename... Extras>
    Scheduler &addLoopOver(std::type_identity<Query<Terms...>>, F &&function, std::string name, size_t grain,
                           internal::List<Extras...>) {
      using Function = std::decay_t<F>;
      using Signature = internal::LoopSignature<typename internal::SystemSignature<Function>::Params>;
      return addSystem(
          [function = Function(std::forward<F>(function)), grain](Query<Terms...> query, Extras... extras) mutable {
            if constexpr (Signature::ENTITY) {
              query.parallelEach(
                  [&](Entity entity, auto &...components) { function(entity, extras..., components...); }, grain);
            } else {
              query.parallelEach([&](auto &...components) { function(extras..., components...); }, grain);
            }
          },
          std::move(name), internal::List<Query<Terms...>, Extras...>{});
    }

    std::vector<System> m_systems;
    std::vector<std::vector<size_t>> m_stages;
    bool m_stagesValid = true;
    // For the commands of a stage, kept for the next one.
    internal::CommandApplier m_applier;
    std::vector<internal::CommandList *> m_stageLists;
  };

} // namespace sc::ecs
