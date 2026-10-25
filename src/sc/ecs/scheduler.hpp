#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "sc/threading/threading.hpp"
#include "sc/util/macros.hpp"

#include "entity.hpp"
#include "resource.hpp"
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

  // The adds to one page of entities, which CommandApplier gives one thread:
  // counted ahead per column (Column::occupyPage) at the first add to it, and
  // again whenever all that was counted is used up, then the count of those
  // not done given back once the page is through, also if a change throws.
  // So two locked instructions per page and column instead of several per
  // add, and only for the adds: modify keeps no count, which cost 1M
  // recorded changes of Health 0.45 ms (11 %) where the count of a unit was
  // kept for every change.
  struct SystemContext;

  // What the hooks of a component make their parameters from on this
  // thread: the system that set them off, or what runs them after the
  // changes of a stage (CommandApplier::runHooked); null outside a system.
  inline thread_local const SystemContext *hookContext = nullptr;

  // Sets hookContext for as long as it lives, unless context is null, and
  // reads it only then: a read of the thread_local on every Edit::erase and
  // Edit::emplace made adding and removing 100K components on all threads
  // take 124 instead of 109 µs (ecs_compare).
  class HookScope {
  public:
    explicit HookScope(const SystemContext *context) noexcept : m_context(context) {
      if (context != nullptr) {
        m_before = hookContext;
        hookContext = context;
      }
    }
    HookScope(const HookScope &) = delete;
    HookScope &operator=(const HookScope &) = delete;
    ~HookScope() {
      if (m_context != nullptr) {
        hookContext = m_before;
      }
    }

  private:
    const SystemContext *m_context;
    const SystemContext *m_before = nullptr;
  };

  class PageAdds {
  public:
    // An add whose column has onAdd hooks, for CommandApplier to run them
    // once all changes are done.
    struct Hooked {
      Entity entity;
      ColumnBase *column;
    };

    // Step is at most the number of changes of the page, as occupyPage
    // brings that many components of a new page into memory. The adds with
    // hooks go to hooked, the components removed with hooks to removed.
    PageAdds(uint32_t number, size_t changes, std::vector<Hooked> &hooked, RemovedStash &removed) noexcept :
        m_number(number), m_step(static_cast<uint32_t>(std::min<size_t>(changes, PAGE_SIZE))), m_hooked(&hooked),
        m_removed(&removed) {}

    PageAdds(const PageAdds &) = delete;
    PageAdds &operator=(const PageAdds &) = delete;

    ~PageAdds() {
      for (const Counted &each: m_counted) {
        each.column->forsakePage(m_number, each.counted - each.added);
      }
    }

    // Before an add to the column. The number of adds done there, to raise
    // for one that is. Inline up to occupy, see CommandApplier::applyChange.
    SC_FORCE_INLINE uint32_t &reserve(ColumnBase &column) {
      const auto found = std::ranges::find(m_counted, &column, &Counted::column);
      if (found == m_counted.end() || found->added == found->counted) [[unlikely]] {
        return occupy(column, found);
      }
      return found->added;
    }

    void hooked(Entity entity, ColumnBase &column) { m_hooked->push_back({.entity = entity, .column = &column}); }

    [[nodiscard]] RemovedStash &removed() const noexcept { return *m_removed; }

  private:
    struct Counted {
      ColumnBase *column;
      uint32_t counted;
      uint32_t added;
    };

    // reserve at the first add to the column, or once all that was counted
    // is used up.
    SC_NOINLINE uint32_t &occupy(ColumnBase &column, std::vector<Counted>::iterator found) {
      if (found == m_counted.end()) {
        m_counted.push_back({.column = &column, .counted = 0, .added = 0});
        found = m_counted.end() - 1;
      }
      column.occupyPage(m_number, m_step);
      found->counted += m_step;
      return found->added;
    }

    // The columns added to, mostly a few.
    std::vector<Counted> m_counted;
    uint32_t m_number;
    uint32_t m_step;
    std::vector<Hooked> *m_hooked;
    RemovedStash *m_removed;
  };

  // Gives the index a component made from the recorded value unless it has
  // one, or only drops the value if page is null; with REPLACE, the one it
  // has gets the value moved in instead. Nothing runs next to it, see
  // CommandApplier. The value is gone afterwards either way, also if making
  // the component throws. Null for a component without members.
  template<typename T, bool REPLACE>
  void finishEmplace(ColumnBase &column, Entity entity, void *value, PageAdds *page) {
    auto &typed = static_cast<Column<T> &>(column);
    const uint32_t index = indexOf(entity);
    // The hooks of an add run once all changes are done, see PageAdds.
    const auto added = [&](bool made) -> uint32_t {
      if (made && typed.hasAddHooks()) [[unlikely]] {
        page->hooked(entity, column);
      }
      return made ? 1 : 0;
    };
    if constexpr (Column<T>::HAS_VALUE) {
      const auto drop = [](T *made) { std::destroy_at(made); };
      const std::unique_ptr<T, decltype(drop)> made{static_cast<T *>(value), drop};
      if constexpr (REPLACE) {
        if (page != nullptr && typed.contains(index)) {
          assert(!typed.replaceForbidden() && "sc::ecs: this component is not replaced, see Column::forbidReplace");
          typed.get(index) = std::move(*made);
          typed.touch(index);
          return;
        }
      }
      if (page != nullptr) {
        uint32_t &count = page->reserve(column);
        count += added(typed.emplaceCounted(index, std::move(*made)));
      }
    } else if (page != nullptr) {
      uint32_t &count = page->reserve(column);
      count += added(typed.emplaceCounted(index));
    }
  }

  // Calls the recorded function with the component at the index if there is
  // one, or only drops the function if page is null. Nothing runs next to
  // it, see CommandList::apply, so without the making bit. The function is
  // gone afterwards either way, also if it throws. TRACKED for a column that
  // tracks changes, chosen when the change is recorded: a look per change
  // here made 1M recorded changes of one page take 4.06 instead of 3.84 ms.
  template<typename T, typename Function, bool TRACKED>
  void finishModify(ColumnBase &column, Entity entity, void *value, PageAdds *page) {
    auto &typed = static_cast<Column<T> &>(column);
    const uint32_t index = indexOf(entity);
    const auto drop = [](Function *function) { std::destroy_at(function); };
    const std::unique_ptr<Function, decltype(drop)> function{static_cast<Function *>(value), drop};
    if (page != nullptr && typed.contains(index)) {
      (*function)(typed.get(index));
      if constexpr (TRACKED) {
        typed.touch(index);
      }
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
      record<&finishEmplace<T, false>>(entity, column, std::forward<Args>(args)...);
    }

    template<typename T, typename... Args>
    void emplaceOrReplace(Entity entity, Column<T> &column, Args &&...args) {
      record<&finishEmplace<T, true>>(entity, column, std::forward<Args>(args)...);
    }

    template<typename T, typename Function>
    void modify(Entity entity, Column<T> &column, Function &&function) {
      using Stored = std::decay_t<Function>;
      // First, as in emplace.
      m_changes.push_back(
          {.entity = entity,
           .column = &column,
           .finish = column.tracksChanges() ? &finishModify<T, Stored, true> : &finishModify<T, Stored, false>,
           .value = nullptr});
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
      // finishEmplace of a component to add or replace, or finishModify of
      // one to modify, which also drop the value; null for one to remove, and
      // once the change is done.
      void (*finish)(ColumnBase &column, Entity entity, void *value, PageAdds *page);
      // The component to add, or the function of modify.
      void *value;
    };

    // What was recorded, for CommandApplier.
    [[nodiscard]] std::span<Change> changes() noexcept { return m_changes; }
    // The page of entities all changes are in, or NO_PAGE if they are in
    // several. Meaningless without changes.
    [[nodiscard]] uint32_t page() const noexcept { return m_page; }
    [[nodiscard]] std::span<const Entity> destroyed() const noexcept { return m_destroyed; }
    [[nodiscard]] bool empty() const noexcept { return m_changes.empty() && m_destroyed.empty(); }

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
          change.finish(*change.column, change.entity, change.value, nullptr);
        }
      }
      m_changes.clear();
      m_values.clear();
      m_destroyed.clear();
    }

    static constexpr uint32_t NO_PAGE = ~uint32_t{0};

  private:
    // A component to add with FINISH, made from the arguments now.
    template<auto FINISH, typename T, typename... Args>
    void record(Entity entity, Column<T> &column, Args &&...args) {
      // First, so that a value that was made is always one of a change.
      m_changes.push_back({.entity = entity, .column = &column, .finish = FINISH, .value = nullptr});
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
  // was recorded), and every page goes to one thread. So the changes of an
  // entity are done in their order, and no other thread touches the page in
  // any column: a page takes no making bit and changes the mask with plain
  // loads and stores, the erases a word at a time (Column::eraseAlone of a
  // KillPage), the adds one at a time (Column::emplaceCounted). Only the
  // count of the page is changed with locked instructions, as a page emptied
  // on another thread may look at it when it leaves the spares: per column
  // twice for the adds (PageAdds) and once per run of erases. Units of a page
  // and a column, which spread a page over the threads by its columns, made
  // 4 components on 1M entities 48 % slower to add and remove (44 instead of
  // 30 ms) and 1M recorded changes of 4 components up to 3 times slower, as
  // grouping by column went through all changes twice more; they were
  // ahead only for functions of modify with real work on fewer pages than
  // threads. The entities are destroyed at once, with
  // World::parallelDestroy.
  class CommandApplier {
  public:
    // Does what the lists recorded and clears them. If a change throws, the
    // pages that are not started yet and the entities to destroy are left in
    // the lists, for clear to drop, and the first exception is rethrown;
    // pages that other threads are working on are finished.
    //
    // The hooks of the world run once the changes of the components are
    // done, on several threads, with their parameters made from the context:
    // first onRemove with the components that were removed, then onAdd, then
    // onRemove of the entities to destroy, which go after them. What the
    // hooks record goes to the lists of the context, which must be others.
    void apply(World &world, std::span<CommandList *const> lists, const SystemContext &context) {
      size_t total = 0;
      for (CommandList *const list: lists) {
        total += list->changes().size();
      }
      if (onePage(lists)) {
        applyInOrder(world, lists, pageOf(lists), total);
      } else {
        // A thread for every so many changes, as World::parallelEach does for
        // entities.
        applySpread(world, lists, total, total / CHANGES_PER_THREAD);
      }

      runStashed(context);
      runHooked(world, context);

      m_destroyed.clear();
      for (CommandList *const list: lists) {
        m_destroyed.insert(m_destroyed.end(), list->destroyed().begin(), list->destroyed().end());
      }
      if (!m_destroyed.empty()) {
        if (world.hasRemoveHooks()) [[unlikely]] {
          runDestroyHooks(world, context);
        }
        world.parallelDestroy(std::span<const Entity>{m_destroyed});
      }
      // All of it done, also the changes of the null entity, which only drop
      // their values.
      for (CommandList *const list: lists) {
        list->clearDone();
      }
    }

    // Forgets the hooks apply had still to run, after an exception: the
    // components moved out for them go.
    void dropHooked() noexcept {
      m_hooked.clear();
      m_running.clear();
      for (RemovedStash &stash: m_stashes) {
        for (const RemovedStash::Removed &removed: stash.items()) {
          removed.column->dropStashed(removed.value);
        }
      }
      m_stashes.clear();
    }

  private:
    // The onAdd hooks of the adds, a thread for so many. A thread takes a
    // page of them at a time, which lie together, as apply does a page at a
    // time: with 64, the threads took the making bits of the same cache
    // lines, and adding and removing 100K components through Commands with
    // an empty hook took 1.17 instead of 0.48 ms (0.34 without the hook).
    void runHooked(World &world, const SystemContext &context) {
      m_running.swap(m_hooked);
      m_hooked.clear();
      threading::parallelFor(m_running.size(), PAGE_SIZE, m_running.size() / CHANGES_PER_THREAD,
                             [&](size_t first, size_t last) {
                               const HookScope scope{&context};
                               for (size_t i = first; i < last; ++i) {
                                 const PageAdds::Hooked &each = m_running[i];
                                 if (world.valid(each.entity)) {
                                   each.column->runAdded(each.entity);
                                 }
                               }
                             });
      m_running.clear();
    }

    // The onRemove hooks of the components moved out, the stash of a thread
    // on one.
    void runStashed(const SystemContext &context) {
      if (m_stashes.empty()) {
        return;
      }
      threading::parallelFor(m_stashes.size(), 1, m_stashes.size(), [&](size_t first, size_t last) {
        const HookScope scope{&context};
        for (size_t i = first; i < last; ++i) {
          for (const RemovedStash::Removed &removed: m_stashes[i].items()) {
            removed.column->runStashed(removed.entity, removed.value);
          }
          m_stashes[i].clear();
        }
      });
      m_stashes.clear();
    }

    // The onRemove hooks of the entities to destroy, once each, in the order
    // of their indices, a page at a time as runHooked.
    void runDestroyHooks(World &world, const SystemContext &context) {
      m_hookDestroyed.assign(m_destroyed.begin(), m_destroyed.end());
      std::ranges::sort(m_hookDestroyed, {}, [](Entity entity) { return indexOf(entity); });
      const auto [last, end] = std::ranges::unique(m_hookDestroyed);
      m_hookDestroyed.erase(last, end);
      threading::parallelFor(m_hookDestroyed.size(), PAGE_SIZE, m_hookDestroyed.size() / CHANGES_PER_THREAD,
                             [&](size_t first, size_t stop) {
                               const HookScope scope{&context};
                               for (size_t i = first; i < stop; ++i) {
                                 if (world.valid(m_hookDestroyed[i])) {
                                   world.runRemoveHooks(m_hookDestroyed[i]);
                                 }
                               }
                             });
    }

    using Change = CommandList::Change;

    // A page is not worth waking a thread for below this.
    static constexpr size_t CHANGES_PER_THREAD = 4096;

    // The erases of a page in one column, gathered until something else comes.
    struct Kill {
      ColumnBase *column;
      // Its removeHooked, so that kill does not look at the column.
      bool hooked;
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

    // The page of the changes, after onePage: none if there are none or they
    // are all of the null entity.
    [[nodiscard]] static uint32_t pageOf(std::span<CommandList *const> lists) noexcept {
      for (CommandList *const list: lists) {
        if (!list->changes().empty()) {
          return list->page() < PAGE_COUNT ? list->page() : CommandList::NO_PAGE;
        }
      }
      return CommandList::NO_PAGE;
    }

    // Every change on the calling thread, in the order of the lists: so many
    // changes, all of the given page.
    void applyInOrder(World &world, std::span<CommandList *const> lists, uint32_t number, size_t changes) {
      std::vector<Kill> kills;
      PageAdds adds{number, changes, m_hooked, m_stashes.emplace_back()};
      for (CommandList *const list: lists) {
        applyList(world, list->changes(), kills, adds);
      }
      flush(kills);
      if (m_stashes.back().items().empty()) {
        m_stashes.pop_back();
      }
    }

    // The changes of one list, in their order. Not inlined: in the loop over
    // the lists GCC kept two of its registers on the stack around every
    // change, which made 1M recorded changes of one page 7 % slower to apply
    // (3.37 instead of 3.15 ms).
    SC_NOINLINE static void applyList(World &world, std::span<Change> changes, std::vector<Kill> &kills,
                                      PageAdds &adds) {
      for (Change &change: changes) {
        applyChange(world, change, kills, adds);
      }
    }

    // Every page on one thread. If a change throws, the pages that are not
    // started yet are left, and the first exception is rethrown once the
    // pages that other threads are working on are finished.
    void applySpread(World &world, std::span<CommandList *const> lists, size_t changes, size_t helpers) {
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
          finish(*change.column, change.entity, change.value, nullptr);
        }
      });
      if (m_sort.pages().size() <= 1) {
        // Only one after all, past those of the null entity. The counts go
        // back to zero first, whatever happens then.
        const uint32_t number = m_sort.pages().empty() ? CommandList::NO_PAGE : m_sort.pages().front();
        m_sort.uncount();
        applyInOrder(world, lists, number, changes);
        return;
      }
      m_sort.place(helpers, index);
      std::atomic_flag failed;
      std::exception_ptr error;
      threading::parallelFor(m_sort.pages().size(), 1, helpers, [&](size_t first, size_t last) {
        std::vector<Kill> kills;
        // Kept for the hooks whichever way the pages are left.
        struct Keep {
          CommandApplier &applier;
          std::vector<PageAdds::Hooked> hooked;
          RemovedStash removed;
          ~Keep() {
            if (!hooked.empty() || !removed.items().empty()) {
              const std::scoped_lock lock{applier.m_hookedMutex};
              applier.m_hooked.insert(applier.m_hooked.end(), hooked.begin(), hooked.end());
              if (!removed.items().empty()) {
                applier.m_stashes.push_back(std::move(removed));
              }
            }
          }
        } keep{.applier = *this, .hooked = {}, .removed = {}};
        for (size_t i = first; i < last; ++i) {
          if (failed.test(std::memory_order_relaxed)) {
            return;
          }
          try {
            applyPage(world, m_sort.page(i), m_sort.pages()[i], kills, keep.hooked, keep.removed);
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
    static void applyPage(World &world, std::span<Change *const> changes, uint32_t number, std::vector<Kill> &kills,
                          std::vector<PageAdds::Hooked> &hooked, RemovedStash &removed) {
      PageAdds adds{number, changes.size(), hooked, removed};
      for (Change *const change: changes) {
        applyChange(world, *change, kills, adds);
      }
      flush(kills);
    }

    // Inlined into applyList and applyPage, with kill and PageAdds::reserve:
    // Clang called all three for every change, and 1M recorded changes of one
    // entity took 155 instead of 140M instructions, adding and removing a
    // component on 100K entities through Commands 45 instead of 37M (GCC
    // inlined them: 128 and 37M either way).
    static SC_FORCE_INLINE void applyChange(World &world, Change &change, std::vector<Kill> &kills, PageAdds &adds) {
      const uint32_t index = indexOf(change.entity);
      // Not that of an entity that got the index since.
      const bool valid = world.valid(change.entity);
      if (change.finish == nullptr) {
        if (valid) {
          kill(kills, *change.column, index, change.entity, adds.removed());
        }
        return;
      }
      flush(kills);
      // Done before the call: finish drops the value whatever happens.
      const auto finish = change.finish;
      change.finish = nullptr;
      finish(*change.column, change.entity, change.value, valid ? &adds : nullptr);
    }

    // An erase for flush; a component with onRemove hooks is moved into the
    // stash first, once, if the entity has it. Both compilers called it
    // where not forced: adding and removing a component on 100K entities
    // through Commands took 39.7 instead of 37.2M instructions with GCC,
    // 39.2 instead of 37.5M with Clang.
    static SC_FORCE_INLINE void kill(std::vector<Kill> &kills, ColumnBase &column, uint32_t index, Entity entity,
                                     RemovedStash &removed) {
      auto found = std::ranges::find(kills, &column, &Kill::column);
      if (found == kills.end()) {
        kills.push_back({.column = &column, .hooked = column.removeHooked, .page = {}});
        found = kills.end() - 1;
      }
      KillPage &page = found->page;
      page.number = index >> PAGE_BITS;
      const uint32_t slot = index & (PAGE_SIZE - 1);
      const uint64_t bit = uint64_t{1} << (slot % 64);
      if (found->hooked && (page.words[slot / 64] & bit) == 0) [[unlikely]] {
        column.stash(entity, removed);
      }
      page.words[slot / 64] |= bit;
      page.touched |= uint64_t{1} << (slot / 64);
    }

    // Does the gathered erases and makes the pages empty again for the next.
    // Before every change that is no erase, so the look stays inline: with
    // applyChange inlined, GCC called all of flush instead, and 1M recorded
    // changes of one entity took 137 instead of 128M instructions.
    static SC_FORCE_INLINE void flush(std::vector<Kill> &kills) noexcept {
      for (Kill &kill: kills) {
        if (kill.page.touched != 0) [[unlikely]] {
          flushPage(kill);
        }
      }
    }

    SC_NOINLINE static void flushPage(Kill &kill) noexcept {
      kill.column->eraseAlone(std::span<const KillPage>{&kill.page, 1});
      for (uint64_t touched = kill.page.touched; touched != 0; touched &= touched - 1) {
        kill.page.words[static_cast<size_t>(std::countr_zero(touched))] = 0;
      }
      kill.page.touched = 0;
    }

    PageSort<Change> m_sort;
    // Of all lists.
    std::vector<Entity> m_destroyed;
    // The adds whose hooks are still to run, and those runHooked is at.
    std::vector<PageAdds::Hooked> m_hooked;
    std::vector<PageAdds::Hooked> m_running;
    // The components removed with hooks, a stash per thread.
    std::vector<RemovedStash> m_stashes;
    std::mutex m_hookedMutex;
    // The entities to destroy for their hooks, once each.
    std::vector<Entity> m_hookDestroyed;
  };

  // Defined below, with the parameters of systems.
  template<typename F>
  struct SystemSignature;

  // The component a function for modify takes: its only parameter.
  template<typename Params>
  struct OnlyParam {
    static_assert(sizeof(Params) == 0, "the function takes the component, and nothing else");
  };
  template<typename Param>
  struct OnlyParam<List<Param>> {
    using Type = std::remove_cvref_t<Param>;
  };

  template<typename F>
  using ModifiedBy = OnlyParam<typename SystemSignature<F>::Params>::Type;

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
    // Changed terms count from the tick since, see World::eachSince: in a
    // system, the end of its last run.
    explicit Query(World &world, uint32_t since = 0) :
        m_world(&world), m_columns(columnsOf(world, typename Lists::GetList{})), m_since(since) {}

    // See World::each.
    template<typename Function>
    void each(Function &&function) const {
      m_world->template eachSince<Terms...>(m_since, function);
    }

    // See World::parallelEach.
    template<typename Function>
    void parallelEach(Function &&function, size_t grain = 1) const {
      m_world->template parallelEachSince<Terms...>(m_since, function, grain);
    }

    // The component of any entity that exists and has it, named the way the
    // query names it. The scheduler keeps other systems from writing it, but
    // not the other threads of the same parallel loop: see World::get.
    // TODO: no static_assert for a component without members, as tryGet has:
    // Column::get forms its address from the null values of such a column.
    template<typename Component>
      requires named<Component>
    [[nodiscard]] Component &get(Entity entity) const {
      assert(m_world->valid(entity));
      auto *const column = std::get<World::ColumnOf<Component> *>(m_columns);
      if constexpr (!std::is_const_v<Component>) {
        // Taken to be written, see World::trackChanges.
        column->touch(indexOf(entity));
      }
      return column->get(indexOf(entity));
    }

    // The same, or null if the entity does not have it or does not exist
    // (any more), see World::tryGet.
    template<typename Component>
      requires named<Component>
    [[nodiscard]] Component *tryGet(Entity entity) const {
      static_assert(World::ColumnOf<Component>::HAS_VALUE,
                    "a component without members has no value to get; ask with contains");
      auto *const column = std::get<World::ColumnOf<Component> *>(m_columns);
      if (!column->contains(indexOf(entity)) || !m_world->valid(entity)) {
        return nullptr;
      }
      if constexpr (!std::is_const_v<Component>) {
        column->touch(indexOf(entity));
      }
      return &column->get(indexOf(entity));
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
    uint32_t m_since;
  };

  // Lets a system make entities, right away and from any thread, also from
  // the function of a parallel loop:
  //
  //   scheduler.addParallel([](Spawn spawn, const Spawner &spawner) { ... spawn.create<Bullet>() ... });
  //
  // An entity without components touches nothing the scheduler knows of, so
  // Spawn keeps no system waiting. To give it components, Edit them.
  class Spawn {
  public:
    explicit Spawn(World &world) : m_world(&world) {}

    // See World::create.
    [[nodiscard]] Entity create() const { return m_world->create(); }

    // The same in the area of a set of components, see World::create: an
    // entity a system spawns lies with the others of its kind.
    template<typename... Set>
      requires(sizeof...(Set) != 0)
    [[nodiscard]] Entity create() const {
      return m_world->template create<Set...>();
    }

  private:
    World *m_world;
  };

  // Lets a system add, remove and change the components it names, on any
  // entity, right away and from several threads for the same one:
  //
  //   scheduler.addParallel([](Edit<Health, Frozen> edit, const Target &target) {
  //     edit.modify(target.entity, [](Health &health) { health.value -= 10; });
  //     edit.emplace<Frozen>(target.entity);
  //   });
  //
  // To the scheduler this is writing the components, so no other system uses
  // them at the same time. Within the system, tryEmplace and the ways of
  // changing a component hold its making bit, so threads that go for the
  // same entity take turns, and erase waits for them; nothing else takes the
  // bit, so nothing else may use the components: no query of the system
  // names them, not even for reading, which is checked when the system is
  // added. The loop of addParallel may go through them, as the function
  // changes them only on its own entity, see World::each; debug builds
  // assert that. To read such a component of another entity, read it in the
  // function of modify.
  //
  // emplace takes no bit and needs no other thread to go for the entity's
  // component, as World::emplace: in a parallel loop, where each thread has
  // entities of its own, for its own entity and for the ones it creates, as
  // long as no thread gives the component to other entities with
  // tryEmplace. Where one does, tryEmplace for all of them.
  //
  // Edit or Commands: where few threads hit the same entity, Edit, which
  // runs right away where the threads are; where many hit few, Commands,
  // which does not touch the entity before the stage is over and applies on
  // several threads, a page on each. Every modify of a thread reaches for the
  // cache line of the lock bit, and every one that takes it writes it twice;
  // with many threads on one entity the line goes from core to core for each
  // of them, whether the bit is free or not. ecs_compare, 1M entities
  // damaging another one, all threads, ns per hit, modify against
  // Commands::modify: random target 1.4 and 3.6, one of 1 000 targets 14.6
  // and 3.2, the same target 15.6 and 3.4.
  //
  // LocalEdit has the same functions without the bit, for systems whose
  // threads never go for the same entity, which is the programmer's to know:
  // on one thread, or in a parallel loop that changes only the entity of the
  // call and the ones it creates. As Edit, not next to a query of the same
  // components. 1M entities each taking 1 off the
  // Health of a random other one, on one thread, ns per hit: Edit::modify
  // 6.4, LocalEdit::modify 2.2, Query::get and writing 1.1 (it does not look
  // whether the entity has one).
  enum class Locking : uint8_t {
    // Takes the making bit, see Edit.
    Bit,
    // Takes none, see LocalEdit.
    None,
  };

  template<Locking LOCKING, typename... Components>
  class BasicEdit {
    static_assert((!std::is_const_v<Components> && ...), "Edit is for writing; a const component is read with Query");

    static constexpr bool LOCKED = LOCKING == Locking::Bit;

    template<typename Component>
    static constexpr bool named = (std::same_as<Component, Components> || ...);

  public:
    // Makes the columns that are not there yet. In a system they are there:
    // Scheduler::run makes them before the first system.
    explicit BasicEdit(World &world) : m_world(&world), m_columns{&world.template column<Components>()...} {}

    // The same in a system, whose parameters the onAdd hooks it sets off get.
    BasicEdit(World &world, const internal::SystemContext *context) : BasicEdit(world) { m_context = context; }

    // An entity with named components, right away, in the area of their set.
    template<typename First, typename... Rest>
      requires(named<First> && (named<Rest> && ...))
    [[nodiscard]] Entity create(const First &first, const Rest &...rest) const {
      const internal::HookScope scope{m_context};
      return m_world->create(first, rest...);
    }

    // The entity must not have the component yet, and no other thread may go
    // for it, see above. Always Emplaced::Made.
    template<typename Component, typename... Args>
      requires named<Component>
    Emplaced emplace(Entity entity, Args &&...args) const { // NOLINT(modernize-use-nodiscard)
      World::ColumnOf<Component> &column = columnFor<Component>(entity);
      const internal::HookScope scope{contextFor(column)};
      return column.emplace(entity, std::forward<Args>(args)...);
    }

    // Gives the entity the component unless it has it: Emplaced::Made, or
    // Emplaced::There if it had it or another thread was first, see
    // World::tryEmplace. Not [[nodiscard]], as World::tryEmplace and
    // World::erase are not: whether this thread was first is often of no
    // interest.
    template<typename Component, typename... Args>
      requires named<Component>
    Emplaced tryEmplace(Entity entity, Args &&...args) const { // NOLINT(modernize-use-nodiscard)
      World::ColumnOf<Component> &column = columnFor<Component>(entity);
      const internal::HookScope scope{contextFor(column)};
      if constexpr (LOCKED) {
        return column.tryEmplace(entity, std::forward<Args>(args)...);
      } else {
        if (column.contains(indexOf(entity))) {
          return Emplaced::There;
        }
        return column.emplace(entity, std::forward<Args>(args)...);
      }
    }

    // See World::emplaceOrReplace.
    template<typename Component, typename... Args>
      requires named<Component>
    Emplaced emplaceOrReplace(Entity entity, Args &&...args) const { // NOLINT(modernize-use-nodiscard)
      World::ColumnOf<Component> &column = columnFor<Component>(entity);
      const internal::HookScope scope{contextFor(column)};
      if constexpr (LOCKED) {
        return column.emplaceOrReplace(entity, std::forward<Args>(args)...);
      } else {
        if (!column.contains(indexOf(entity))) {
          return column.emplace(entity, std::forward<Args>(args)...);
        }
        assert(!column.replaceForbidden() && "sc::ecs: this component is not replaced, see Column::forbidReplace");
        if constexpr (!std::is_empty_v<Component>) {
          changed(column, entity) = Component{std::forward<Args>(args)...};
        }
        return Emplaced::Replaced;
      }
    }

    // See World::modify. The component is the one the function takes, by
    // reference.
    template<typename Function>
    bool modify(Entity entity, Function &&function) const { // NOLINT(modernize-use-nodiscard)
      using Component = internal::ModifiedBy<std::decay_t<Function>>;
      static_assert(named<Component>, "the function takes a component the Edit names");
      World::ColumnOf<Component> &column = columnFor<Component>(entity);
      if constexpr (LOCKED) {
        [[maybe_unused]] const internal::Modifying modifying;
        return column.modify(indexOf(entity), std::forward<Function>(function));
      } else {
        if (!column.contains(indexOf(entity))) {
          return false;
        }
        function(changed(column, entity));
        return true;
      }
    }

    // See World::emplaceOrModify. The component is the one the function
    // takes, by reference; it is made from the arguments if the entity has
    // none.
    template<typename Function, typename... Args>
    Emplaced emplaceOrModify(Entity entity, Function &&function, // NOLINT(modernize-use-nodiscard)
                             Args &&...args) const {
      using Component = internal::ModifiedBy<std::decay_t<Function>>;
      static_assert(named<Component>, "the function takes a component the Edit names");
      World::ColumnOf<Component> &column = columnFor<Component>(entity);
      const internal::HookScope scope{contextFor(column)};
      if constexpr (LOCKED) {
        [[maybe_unused]] const internal::Modifying modifying;
        return column.emplaceOrModify(entity, std::forward<Function>(function), std::forward<Args>(args)...);
      } else {
        if (!column.contains(indexOf(entity))) {
          return column.emplace(entity, std::forward<Args>(args)...);
        }
        function(changed(column, entity));
        return Emplaced::Modified;
      }
    }

    template<typename Component>
      requires named<Component>
    bool erase(Entity entity) const { // NOLINT(modernize-use-nodiscard)
      World::ColumnOf<Component> &column = columnFor<Component>(entity);
      if (column.hasHooks()) [[unlikely]] {
        const internal::HookScope scope{m_context};
        return eraseFrom(column, entity);
      }
      return eraseFrom(column, entity);
    }

    // The component of the entity, without a look whether it has one, see
    // World::get; a const one only to read. Only LocalEdit: through Edit it
    // would get past the bit that modify of other threads takes.
    template<typename Component>
      requires(named<std::remove_const_t<Component>> && !LOCKED)
    [[nodiscard]] Component &get(Entity entity) const {
      static_assert(World::ColumnOf<Component>::HAS_VALUE,
                    "a component without members has no value to get; ask with contains");
      auto &column = columnFor<std::remove_const_t<Component>>(entity);
      if constexpr (!std::is_const_v<Component>) {
        column.touch(indexOf(entity));
      }
      return column.get(indexOf(entity));
    }

    // The same, or null if the entity does not have it or does not exist
    // (any more), see World::tryGet.
    template<typename Component>
      requires(named<std::remove_const_t<Component>> && !LOCKED)
    [[nodiscard]] Component *tryGet(Entity entity) const {
      static_assert(World::ColumnOf<Component>::HAS_VALUE,
                    "a component without members has no value to get; ask with contains");
      auto *const column = std::get<World::ColumnOf<std::remove_const_t<Component>> *>(m_columns);
      if (!column->contains(indexOf(entity)) || !m_world->valid(entity)) {
        return nullptr;
      }
      if constexpr (!std::is_const_v<Component>) {
        column->touch(indexOf(entity));
      }
      return &column->get(indexOf(entity));
    }

    // Any entity may be asked for, as World::contains.
    template<typename Component>
      requires named<Component>
    [[nodiscard]] bool contains(Entity entity) const {
      return std::get<World::ColumnOf<Component> *>(m_columns)->contains(indexOf(entity)) && m_world->valid(entity);
    }

  private:
    // The column of a named component, looked up once when the Edit is made,
    // as Query does: through World::column on every call, adding and removing
    // a component on all threads in addParallel took 151 to 156 instead of 136
    // to 138 µs (ecs_compare, 100K entities). The entity must exist, which only
    // debug builds check, as World does.
    template<typename Component>
    [[nodiscard]] World::ColumnOf<Component> &columnFor([[maybe_unused]] Entity entity) const {
      assert(m_world->valid(entity));
      return *std::get<World::ColumnOf<Component> *>(m_columns);
    }

    template<typename Column>
    static bool eraseFrom(Column &column, Entity entity) noexcept {
      if constexpr (LOCKED) {
        // Waits for the bit of a component that DESTROYS, see Column::erase.
        return column.erase(entity);
      } else {
        return column.eraseWithoutBit(entity);
      }
    }

    // The system for the hooks of the column, if it has any: setting
    // hookContext on every call is a write per add.
    template<typename Column>
    [[nodiscard]] const internal::SystemContext *contextFor(const Column &column) const noexcept {
      return column.hasHooks() ? m_context : nullptr;
    }

    // The component of the entity, to change without the bit, checked as
    // modify is: not one a loop names on another entity.
    template<typename Column>
    [[nodiscard]] static auto &changed(Column &column, Entity entity) noexcept {
      internal::assertNotLooped(&column, indexOf(entity));
      column.touch(indexOf(entity));
      return column.get(indexOf(entity));
    }

    World *m_world;
    std::tuple<World::ColumnOf<Components> *...> m_columns;
    const internal::SystemContext *m_context = nullptr;
  };

  template<typename... Components>
  using Edit = BasicEdit<Locking::Bit, Components...>;

  template<typename... Components>
  using LocalEdit = BasicEdit<Locking::None, Components...>;

  // Records what a system cannot do right away: destroying entities, which
  // touches every component, and removing a component another thread may be
  // using. Adding a component or changing its value is recorded for a system
  // that would otherwise have to write it with Edit, and keep the systems
  // that use it waiting, and to need no lock.
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
      withList(m_lists, [&](internal::CommandList &list) { list.destroy(entity); });
    }

    // Makes the component now, from the arguments, and gives it to the entity
    // later. Does nothing if the entity does not exist any more by then or
    // has the component already: the one that was made is dropped. Like
    // erase, this does not count as writing the component.
    template<typename Component, typename... Args>
    void emplace(Entity entity, Args &&...args) const {
      auto &column = m_world->template column<Component>();
      withList(m_lists,
               [&](internal::CommandList &list) { list.emplace(entity, column, std::forward<Args>(args)...); });
    }

    // The same, but an entity that has the component by then gets the made
    // one moved into it, as World::emplaceOrReplace does now:
    //
    //   commands.emplaceOrReplace<State>(entity, State::Dead);
    //
    // Of several recorded for one component of an entity, the last one done
    // stays, see modify for their order. Like emplace, this does not count as
    // writing the component: the systems that use it run at the same time
    // and see it as it was.
    template<typename Component, typename... Args>
    void emplaceOrReplace(Entity entity, Args &&...args) const {
      auto &column = m_world->template column<Component>();
      withList(m_lists, [&](internal::CommandList &list) {
        list.emplaceOrReplace(entity, column, std::forward<Args>(args)...);
      });
    }

    // Does nothing if the entity does not exist any more by then or does not
    // have the component. A system that does this does not count as writing
    // the component: the systems that use it run at the same time and still
    // find it.
    template<typename Component>
    void erase(Entity entity) const {
      auto &column = m_world->template column<Component>();
      withList(m_lists, [&](internal::CommandList &list) { list.erase(entity, column); });
    }

    // Calls the function with the component of the entity later, as
    // Edit::modify does now:
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
    // Faster than Edit::modify where many threads hit few entities, slower
    // where they hit many, see Edit.
    template<typename Function>
    void modify(Entity entity, Function &&function) const {
      using Component = internal::ModifiedBy<std::decay_t<Function>>;
      static_assert(std::is_invocable_v<std::decay_t<Function> &, Component &>,
                    "the function takes the component by reference");
      static_assert(internal::Column<Component>::HAS_VALUE, "a component without members has no value to modify");
      auto &column = m_world->template column<Component>();
      withList(m_lists,
               [&](internal::CommandList &list) { list.modify(entity, column, std::forward<Function>(function)); });
    }

  private:
    // Calls the function with the list of the calling thread. The last list
    // is shared by the threads outside the pool, which lock it for the call.
    //
    // Static, with the lists given: as a member, the address of the Commands
    // went to the instances that stayed calls, so a loop that takes Commands
    // by value copied them to the stack for every entity, and Clang did so
    // before the function looked at the entity. 1M entities on all threads
    // of which none records anything (the churn of ecs_compare): Clang 84
    // instead of 148 µs, GCC 86 µs either way; recording the same.
    template<typename Function>
    static void withList(std::span<internal::CommandList> lists, Function &&function) {
      const uint32_t thread = threading::threadId();
      if (thread < lists.size() - 1) [[likely]] {
        function(lists[thread]);
        return;
      }
      internal::CommandList &shared = lists.back();
      const std::scoped_lock lock{shared.mutex()};
      function(shared);
    }

    World *m_world;
    // One for every sc::threading::threadId, and one more for the threads
    // outside the pool.
    std::span<internal::CommandList> m_lists;
  };

  // The ResourceCache given to the scheduler with provide, as a parameter of
  // a system: Resources<const T> gives the const members (bind, find, get),
  // Resources<T> all of them, and the scheduler keeps a system that takes
  // Resources<T> apart from the others that take the cache. Only
  // Resources<const T> in the function of addParallel, in a hook and in a
  // listener of an event a loop emits, as for Global.
  //
  //   ResourceCache<Texture> textures([](h64 key) { return Texture(assets.path(key)); });
  //   scheduler.provide(textures);
  //   scheduler.addParallel([](Resources<const Texture> textures, Commands commands, Entity entity,
  //                            const SpawnRock &) {
  //     commands.emplace<Material>(entity, textures->bind("textures/rock"_h));
  //   });
  //   scheduler.add([](Resources<Texture> textures) { textures->collect(); }, "collect textures");
  //
  // After every stage, run takes the keys bound in it into the map that find
  // and get look into without a lock (ResourceCache::apply). A copy is one
  // pointer.
  template<typename T, typename Key = h64, typename Hash = TransparentHash>
  class Resources {
  public:
    using Cache = ResourceCache<std::remove_const_t<T>, Key, Hash>;
    using Pointer = std::conditional_t<std::is_const_v<T>, const Cache *, Cache *>;

    explicit Resources(Pointer cache) noexcept : m_cache(cache) {}

    [[nodiscard]] Pointer operator->() const noexcept { return m_cache; }
    [[nodiscard]] auto &operator*() const noexcept { return *m_cache; }

  private:
    Pointer m_cache;
  };

  // A global value of the world (World::setGlobal) as a parameter of a
  // system: Global<const T> reads it, Global<T> writes it, and the scheduler
  // keeps a system that writes it apart from the others that use it. In the
  // function of addParallel only Global<const T>, as all threads of the loop
  // would write the one value; the same goes for a listener of an event a
  // loop emits. A system of add that runs a parallelEach of its own is not
  // checked: writing the value from that loop is its own to keep apart.
  // std::logic_error when the system runs on a world without the value.
  //
  //   scheduler.add([](Global<GameTime> time) { time->seconds += 1.0f / 60.0f; });
  //   scheduler.addParallel([](Global<const GameTime> time, Position &pos, const Velocity &vel) { ... });
  template<typename T>
  class Global {
    static_assert(std::is_same_v<std::remove_const_t<T>, std::remove_cvref_t<T>>,
                  "Global names the type of the value, const to only read it");

  public:
    explicit Global(T &value) noexcept : m_value(&value) {}

    [[nodiscard]] T *operator->() const noexcept { return m_value; }
    [[nodiscard]] T &operator*() const noexcept { return *m_value; }

  private:
    T *m_value;
  };

} // namespace sc::ecs

namespace sc::ecs::internal {

  // Stands for the type of a cache given with Scheduler::provide: its
  // address, the same in every translation unit.
  template<typename Cache>
  inline constexpr char cacheTag = 0;

  struct ProvidedCache {
    const void *tag;
    void *cache;
    // ResourceCache::apply of the cache, after every stage.
    size_t (*apply)(void *cache);
  };

  // Stands for the type of an event of Scheduler::on and Emit, as cacheTag.
  template<typename Event>
  inline constexpr char eventTag = 0;

  // Makes the column of a component in a world, for Scheduler::run.
  struct ColumnMaker {
    size_t index;
    bool (*made)(const World &);
    void (*make)(World &);
  };

  template<typename Component>
  [[nodiscard]] ColumnMaker columnMaker() {
    return {.index = componentIndex<Component>(),
            .made = [](const World &world) { return world.template findColumn<Component>() != nullptr; },
            .make = [](World &world) { (void) world.template column<Component>(); }};
  }

  // What a system reads and writes, collected from its parameters.
  struct SystemAccess {
    struct Component {
      size_t index;
      bool write;
    };

    std::vector<Component> components;
    // The same for global values, by internal::globalIndex.
    std::vector<Component> globals;
    // The caches given with provide, by cacheTag.
    struct Cache {
      const void *tag;
      bool write;
    };
    std::vector<Cache> caches;
    // The system takes the world itself and may do anything with it.
    bool exclusive = false;
    bool commands = false;
    // The events it emits, by eventTag: what their listeners use is added
    // when the stages are built.
    std::vector<const void *> emits;
    // The components it takes through Edit and through queries, not the one
    // of its loop, which must not overlap: a loop of the query would get to
    // an edited entity or not, depending on where it lies.
    std::vector<size_t> modified;
    std::vector<size_t> gotten;
    // Of these, the ones taken through LocalEdit, whose threads must not
    // meet a hook or listener that edits them; not merged.
    std::vector<size_t> localEdited;
    // The columns its parameters look up when they are made or used.
    std::vector<ColumnMaker> columns;

    // Several parameters may name the same component; writing wins.
    void add(size_t index, bool write) { addTo(components, index, write); }

    void addGlobal(size_t index, bool write) { addTo(globals, index, write); }

    [[nodiscard]] bool writesGlobal() const noexcept { return std::ranges::any_of(globals, &Component::write); }

    void addCache(const void *tag, bool write) {
      for (Cache &cache: caches) {
        if (cache.tag == tag) {
          cache.write = cache.write || write;
          return;
        }
      }
      caches.push_back({.tag = tag, .write = write});
    }

    [[nodiscard]] bool writesCache() const noexcept { return std::ranges::any_of(caches, &Cache::write); }

    void addColumn(const ColumnMaker &maker) {
      if (std::ranges::find(columns, maker.index, &ColumnMaker::index) == columns.end()) {
        columns.push_back(maker);
      }
    }

    void addEmit(const void *tag) {
      if (std::ranges::find(emits, tag) == emits.end()) {
        emits.push_back(tag);
      }
    }

    // Uses what the other one uses as well.
    void merge(const SystemAccess &other) {
      for (const Component &component: other.components) {
        add(component.index, component.write);
      }
      for (const Component &global: other.globals) {
        addGlobal(global.index, global.write);
      }
      for (const Cache &cache: other.caches) {
        addCache(cache.tag, cache.write);
      }
      exclusive = exclusive || other.exclusive;
      commands = commands || other.commands;
      for (const void *const tag: other.emits) {
        addEmit(tag);
      }
      modified.insert(modified.end(), other.modified.begin(), other.modified.end());
      gotten.insert(gotten.end(), other.gotten.begin(), other.gotten.end());
      for (const ColumnMaker &maker: other.columns) {
        addColumn(maker);
      }
    }

    // Whether a component taken through Edit is also taken past it.
    // TODO: not checked, here or in usesModifiedOtherwise: a loop that writes
    // Health with a Query<const Health> (or a listener with one) reads the
    // Health of other entities while other threads of the loop write it.
    [[nodiscard]] bool modifiesGotten() const noexcept {
      return std::ranges::any_of(modified,
                                 [this](size_t index) { return std::ranges::find(gotten, index) != gotten.end(); });
    }

    // Whether the two must not run at the same time.
    [[nodiscard]] bool conflictsWith(const SystemAccess &other) const noexcept {
      return exclusive || other.exclusive || overlap(components, other.components) || overlap(globals, other.globals) ||
             overlapCaches(caches, other.caches);
    }

  private:
    static void addTo(std::vector<Component> &list, size_t index, bool write) {
      for (Component &component: list) {
        if (component.index == index) {
          component.write = component.write || write;
          return;
        }
      }
      list.push_back({.index = index, .write = write});
    }

    [[nodiscard]] static bool overlapCaches(const std::vector<Cache> &mine, const std::vector<Cache> &theirs) noexcept {
      for (const Cache &one: mine) {
        for (const Cache &other: theirs) {
          if (one.tag == other.tag && (one.write || other.write)) {
            return true;
          }
        }
      }
      return false;
    }

    // Whether one of the two writes something the other one uses.
    [[nodiscard]] static bool overlap(const std::vector<Component> &mine,
                                      const std::vector<Component> &theirs) noexcept {
      for (const Component &one: mine) {
        for (const Component &other: theirs) {
          if (one.index == other.index && (one.write || other.write)) {
            return true;
          }
        }
      }
      return false;
    }
  };

  struct SystemContext;

  // A function given to Scheduler::on, called with the event and the context
  // of the system that emits it, on several threads at once.
  struct Listener {
    std::move_only_function<void(const void *, const SystemContext &) const> call;
    SystemAccess access;
  };

  // The listeners of one type of event.
  struct EventListeners {
    const void *tag;
    std::vector<Listener> listeners;
  };

  // What the parameters of a system are made from when it runs.
  struct SystemContext {
    World &world;
    std::span<CommandList> commands;
    std::span<const ProvidedCache> caches;
    std::span<const EventListeners> events;
    // Of the system, for errors.
    const std::string &name;
    // The tick of the clock of the world at the end of the system's last
    // run, for its queries with Changed terms; 0 before the first.
    uint32_t since = 0;
  };

} // namespace sc::ecs::internal

namespace sc::ecs {

  // Calls the listeners of Event given to the scheduler with on, right away
  // and on the calling thread, as a parameter of a system:
  //
  //   scheduler.on([](const Hit &hit, Edit<Health> edit) {
  //     edit.modify(hit.target, [&](Health &health) { health.value -= hit.damage; });
  //   });
  //   scheduler.addParallel([](Emit<Hit> emit, const Weapon &weapon) { emit(Hit{weapon.target, weapon.damage}); });
  //
  // The system counts as using what the listeners use, of the events they
  // emit as well, so the scheduler keeps it apart from the systems they would
  // race with. In a loop the listeners run on all its threads at once. Only
  // valid while the system runs.
  template<typename Event>
  class Emit {
    static_assert(std::is_same_v<Event, std::remove_cvref_t<Event>>, "Emit names the type of the event");

  public:
    Emit(const internal::EventListeners *listeners, const internal::SystemContext &context) noexcept :
        m_listeners(listeners), m_context(&context) {}

    // The parameters of every listener are made for every event.
    void operator()(const Event &event) const {
      if (m_listeners == nullptr) {
        return;
      }
      for (const internal::Listener &listener: m_listeners->listeners) {
        listener.call(&event, *m_context);
      }
    }

  private:
    // Null if the event has none.
    const internal::EventListeners *m_listeners;
    const internal::SystemContext *m_context;
  };

} // namespace sc::ecs

namespace sc::ecs::internal {

  // How one kind of system parameter is declared to the scheduler and how its
  // value is made when the system runs.
  template<typename Param>
  struct SystemParam;

  template<typename... Terms>
  struct SystemParam<Query<Terms...>> {
    static void declare(SystemAccess &access) {
      using Lists = Split<List<>, List<>, Terms...>;
      declare(access, typename Lists::GetList{}, typename Lists::ExcludedList{}, ChangedList<Terms...>{});
    }

    static Query<Terms...> fetch(const SystemContext &context) { return Query<Terms...>{context.world, context.since}; }

  private:
    template<typename... Get, typename... Excluded, typename... Changes>
    static void declare(SystemAccess &access, List<Get...>, List<Excluded...>, List<Changes...>) {
      (access.add(componentIndex<std::remove_const_t<Get>>(), !std::is_const_v<Get>), ...);
      (access.gotten.push_back(componentIndex<std::remove_const_t<Get>>()), ...);
      (access.addColumn(columnMaker<std::remove_const_t<Get>>()), ...);
      // An excluded component is only looked up, never changed.
      (access.add(componentIndex<std::remove_const_t<Excluded>>(), false), ...);
      // Changed reads the ticks of the component and its mask: no system that
      // writes it runs next to the query, so the end of its run is a tick no
      // write of another one falls behind, see World::tick.
      (access.add(componentIndex<std::remove_const_t<Changes>>(), false), ...);
      (access.gotten.push_back(componentIndex<std::remove_const_t<Changes>>()), ...);
    }
  };

  template<Locking LOCKING, typename... Components>
  struct SystemParam<BasicEdit<LOCKING, Components...>> {
    static void declare(SystemAccess &access) {
      (access.add(componentIndex<Components>(), true), ...);
      (access.modified.push_back(componentIndex<Components>()), ...);
      (access.addColumn(columnMaker<Components>()), ...);
      if constexpr (LOCKING == Locking::None) {
        (access.localEdited.push_back(componentIndex<Components>()), ...);
      }
    }

    static BasicEdit<LOCKING, Components...> fetch(const SystemContext &context) {
      return BasicEdit<LOCKING, Components...>{context.world, &context};
    }
  };

  template<>
  struct SystemParam<Spawn> {
    // A new entity has nothing anybody else uses.
    static void declare(SystemAccess &) {}

    static Spawn fetch(const SystemContext &context) { return Spawn{context.world}; }
  };

  template<>
  struct SystemParam<Commands> {
    static void declare(SystemAccess &access) { access.commands = true; }

    static Commands fetch(const SystemContext &context) {
      if (context.commands.empty()) {
        throw std::logic_error("\"" + context.name + "\" takes Commands outside a system");
      }
      return Commands{context.world, context.commands};
    }
  };

  template<>
  struct SystemParam<World &> {
    static void declare(SystemAccess &access) { access.exclusive = true; }

    static World &fetch(const SystemContext &context) { return context.world; }
  };

  template<typename T, typename Key, typename Hash>
  struct SystemParam<Resources<T, Key, Hash>> {
    using Cache = Resources<T, Key, Hash>::Cache;

    static void declare(SystemAccess &access) { access.addCache(&cacheTag<Cache>, !std::is_const_v<T>); }

    // One look through the provided caches per run of the system, not per
    // entity.
    static Resources<T, Key, Hash> fetch(const SystemContext &context) {
      for (const ProvidedCache &provided: context.caches) {
        if (provided.tag == &cacheTag<Cache>) {
          return Resources<T, Key, Hash>(static_cast<Cache *>(provided.cache));
        }
      }
      throw std::logic_error("the system \"" + context.name + "\" takes a cache the scheduler was not provided with");
    }
  };

  template<typename T>
  struct SystemParam<Global<T>> {
    static void declare(SystemAccess &access) {
      access.addGlobal(globalIndex<std::remove_const_t<T>>(), !std::is_const_v<T>);
    }

    // One look per run of the system, not per entity.
    static Global<T> fetch(const SystemContext &context) {
      T *const value = context.world.template tryGlobal<std::remove_const_t<T>>();
      if (value == nullptr) {
        throw std::logic_error("the system \"" + context.name + "\" takes a global value the world does not have");
      }
      return Global<T>(*value);
    }
  };

  // Whether a parameter of a system writes a global value.
  template<typename Param>
  inline constexpr bool writesGlobal = false;
  template<typename T>
  inline constexpr bool writesGlobal<Global<T>> = !std::is_const_v<T>;

  // Whether a parameter of a system takes the members of a cache that run
  // alone.
  template<typename Param>
  inline constexpr bool writesCache = false;
  template<typename T, typename Key, typename Hash>
  inline constexpr bool writesCache<Resources<T, Key, Hash>> = !std::is_const_v<T>;

  template<typename Event>
  struct SystemParam<Emit<Event>> {
    static void declare(SystemAccess &access) { access.addEmit(&eventTag<Event>); }

    static Emit<Event> fetch(const SystemContext &context) {
      for (const EventListeners &listeners: context.events) {
        if (listeners.tag == &eventTag<Event>) {
          return Emit<Event>(&listeners, context);
        }
      }
      return Emit<Event>(nullptr, context);
    }
  };

  // Queries, Edit and Commands are taken by value or by const reference, the
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
  template<Locking LOCKING, typename... Components>
  inline constexpr bool isLoopExtra<BasicEdit<LOCKING, Components...>> = true;
  template<>
  inline constexpr bool isLoopExtra<Spawn> = true;
  template<>
  inline constexpr bool isLoopExtra<Commands> = true;
  template<typename... Terms>
  inline constexpr bool isLoopExtra<Query<Terms...>> = true;
  template<typename T, typename Key, typename Hash>
  inline constexpr bool isLoopExtra<Resources<T, Key, Hash>> = true;
  template<typename Event>
  inline constexpr bool isLoopExtra<Emit<Event>> = true;
  template<typename T>
  inline constexpr bool isLoopExtra<Global<T>> = true;

  // The parameters of a function for Scheduler::addParallel: the entity, then
  // Edit, Spawn, Commands, queries, Resources, Emit and Global, all of them
  // optional, and then the components.
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

  // The components a parameter of a system names for Edit.
  template<typename Param>
  struct ModifiedOf {
    using Type = List<>;
  };
  template<Locking LOCKING, typename... Components>
  struct ModifiedOf<BasicEdit<LOCKING, Components...>> {
    using Type = List<Components...>;
  };

  // The components a parameter of a system reads or writes without Edit:
  // those a query gets. Excluded ones are only looked up in the mask, which
  // modify does not change.
  template<typename Param>
  struct GottenOf {
    using Type = List<>;
  };
  template<typename... Terms>
  struct GottenOf<Query<Terms...>> {
    using Type = Split<List<>, List<>, Terms...>::GetList;
  };

  template<typename Component, typename... Gotten>
  inline constexpr bool isGotten = (std::same_as<Component, std::remove_const_t<Gotten>> || ...);

  template<typename Modified, typename Gotten>
  inline constexpr bool overlaps = false;
  template<typename... Modified, typename... Gotten>
  inline constexpr bool overlaps<List<Modified...>, List<Gotten...>> = (isGotten<Modified, Gotten...> || ...);

  // Whether a system with these parameters uses a component it edits also
  // through a query, whose loop would get to an edited entity or not,
  // depending on where it lies.
  // Every typename here is needed: these are template arguments, where C++20
  // does not assume a type. clang-tidy 22 finds them redundant all the same.
  template<typename... Params>
  // NOLINTBEGIN(readability-redundant-typename)
  inline constexpr bool usesModifiedOtherwise =
      overlaps<typename Concat<typename ModifiedOf<std::remove_cvref_t<Params>>::Type...>::Type,
               typename Concat<typename GottenOf<std::remove_cvref_t<Params>>::Type...>::Type>;
  // NOLINTEND(readability-redundant-typename)

  // The same for the parameters of a system, the first of which is the
  // query of its loop when LOOP is true: the loop may go through what the
  // system edits, as its function changes it only on its own entity.
  template<bool LOOP, typename... Params>
  inline constexpr bool editsQueried = usesModifiedOtherwise<Params...>;
  template<typename Loop, typename... Rest>
  inline constexpr bool editsQueried<true, Loop, Rest...> = usesModifiedOtherwise<Rest...>;

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
  //   void hit(Query<const Target> query, Edit<Health, Frozen> edit);   // adds, removes and changes them, locked
  //   void spawn(Query<const Spawner> query, Spawn spawn);   // makes entities
  //   void reap(Query<const Lifetime> query, Commands commands);   // destroys entities, later
  //   void stream(Resources<Texture> textures);            // loads into a cache given with provide
  //   void tick(Global<GameTime> time);                    // writes a global value of the world
  //   void fight(Query<const Weapon> query, Emit<Hit> emit);   // uses what the listeners of Hit use
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
    // after it, Edit, Spawn, Commands, queries, Resources, Emit and
    // Global<const T>, Edit and queries for entities other than its own:
    //
    //   scheduler.addParallel([](Entity entity, Commands commands, const Health &health) { ... });
    //   scheduler.addParallel([](Query<const Health> others, const Target &target) { ... });
    //   scheduler.addParallel([](Edit<Health> edit, const Target &target) { ... });
    //
    // Writing through a query there is the function's to keep apart from the
    // other threads of the loop, which may be at the same entity, see
    // World::get; Edit::modify takes turns with them.
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

    // Makes the cache the one systems get as Resources<T, Key, Hash> and
    // Resources<const T, Key, Hash>, and the one run applies after every
    // stage. Only a pointer is kept: the cache has to outlive every run. One cache per
    // type; owners within it are for resources that go at points of their
    // own. std::logic_error for a second cache of the same type. Not during
    // run.
    template<typename T, typename Key, typename Hash>
    Scheduler &provide(ResourceCache<T, Key, Hash> &cache) {
      const void *const tag = &internal::cacheTag<ResourceCache<T, Key, Hash>>;
      for (const internal::ProvidedCache &provided: m_caches) {
        if (provided.tag == tag) {
          throw std::logic_error("the scheduler has a cache of this type already");
        }
      }
      // TODO: not clang-formatted since 57375cf; clang-format-check of CI fails on it.
      m_caches.push_back({.tag = tag,
                          .cache = &cache,
                          .apply = [](void *provided) { return static_cast<ResourceCache<T, Key, Hash> *>(provided)->apply(); }});
      return *this;
    }

    // Adds a listener of the event its first parameter takes, by value or
    // const reference. The parameters after it are those of a system. It is
    // called where a system emits the event with Emit, right away and on that
    // thread, so on several threads at once for a loop, and through a const
    // reference: a mutable lambda does not compile.
    //
    //   scheduler.on([](const Hit &hit, Edit<Health> edit) { ... });
    //   scheduler.on([](const Died &died, Commands commands) { commands.destroy(died.entity); });
    //
    // The systems that emit the event count as using what its listeners use,
    // so a listener with nothing but the event makes them run alone, as a
    // system without parameters does. Listeners may be added after the
    // systems that emit their event, not during run.
    template<typename F>
    Scheduler &on(F &&listener) {
      return addListener(std::forward<F>(listener), typename internal::SystemSignature<std::decay_t<F>>::Params{});
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
      const std::vector<std::vector<size_t>> &stages = this->stages(world);

      // Here and not when the system is added: the pool may be set up later.
      const size_t threads = threading::threadSlots();
      for (System &system: m_systems) {
        // One more for the threads outside the pool, see Commands::withList.
        if (system.effective.commands && system.commands.size() < threads + 1) {
          system.commands = std::vector<internal::CommandList>(threads + 1);
        }
      }
      makeColumns(world);
      for (std::vector<internal::CommandList> &lists: m_hookLists) {
        if (lists.size() < threads + 1) {
          lists = std::vector<internal::CommandList>(threads + 1);
        }
      }

      for (const std::vector<size_t> &stage: stages) {
        try {
          // runAll puts more than one system on the pool.
          [[maybe_unused]] const internal::ParallelRun run = world.parallelRun(stage.size() > 1);
          internal::runAll(stage, [this, &world](size_t index) {
            System &system = m_systems[index];
            world.advanceTick();
            system.run({.world = world,
                        .commands = system.commands,
                        .caches = m_caches,
                        .events = m_events,
                        .name = system.name,
                        .since = system.lastRun});
            system.lastRun = world.tick();
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
          // After the end of every system of the stage, see SystemContext::since.
          world.advanceTick();
          m_applier.apply(world, m_stageLists, hookContext(world, 0));
          runHookRounds(world);
          // After the hooks, which may bind keys as well.
          for (const internal::ProvidedCache &provided: m_caches) {
            provided.apply(provided.cache);
          }
        } catch (...) {
          clearCommands(stage);
          for (std::vector<internal::CommandList> &lists: m_hookLists) {
            for (internal::CommandList &list: lists) {
              list.clear();
            }
          }
          m_applier.dropHooked();
          throw;
        }
      }
    }

    [[nodiscard]] size_t size() const noexcept { return m_systems.size(); }

    [[nodiscard]] const std::string &name(size_t system) const { return m_systems[system].name; }

    // The systems in the order they run, by index in the order they were
    // added: one stage after the other, the systems of a stage at the same
    // time.
    //
    // std::logic_error for a system that takes a component through a query
    // which it or a listener of an event it emits edits, as addSystem's
    // static_assert for a system alone, and for a loop of addParallel with a
    // listener that writes a global value, as addLoopOver's for the loop.
    // Without the hooks of a world, see the next one.
    [[nodiscard]] const std::vector<std::vector<size_t>> &stages() { return stagesFor(nullptr); }

    // The same with the hooks of the world (World::onAdd), what run uses: a
    // system counts as using what the hooks of the components it edits use.
    // std::logic_error also for a system that takes a component through
    // LocalEdit which a hook or listener it sets off edits, as their threads
    // may meet at an entity without the bit, and for hooks that edit a
    // component other hooks take through a query: those of the adds through
    // Commands run at the same time.
    [[nodiscard]] const std::vector<std::vector<size_t>> &stages(const World &world) { return stagesFor(&world); }

  private:
    [[nodiscard]] const std::vector<std::vector<size_t>> &stagesFor(const World *world) {
      const uint64_t hookVersion = world != nullptr ? world->hookVersion() : 0;
      if (world != m_stagesWorld || hookVersion != m_stagesHookVersion) {
        m_stagesValid = false;
      }
      if (!m_stagesValid) {
        for (System &system: m_systems) {
          std::vector<size_t> foreign;
          system.effective = effectiveAccess(system.access, world, foreign);
          for (const size_t component: system.access.localEdited) {
            if (std::ranges::find(foreign, component) != foreign.end()) {
              throw std::logic_error("the system \"" + system.name +
                                     "\" takes a component through LocalEdit that a hook or listener it sets off "
                                     "edits as well: their threads may meet at an entity without the bit; take it "
                                     "through Edit");
            }
          }
          if (system.effective.modifiesGotten()) {
            throw std::logic_error("the system \"" + system.name +
                                   "\" or a listener of an event it emits edits a component that one of them also "
                                   "takes through a query: a loop of the query would get to an edited entity or not, "
                                   "depending on where it lies");
          }
          // The loop itself cannot, see addLoopOver.
          if (system.loop && system.effective.writesGlobal()) {
            throw std::logic_error("a listener of an event the loop \"" + system.name +
                                   "\" emits writes a global value: all threads of the loop would write the one "
                                   "value; take it as Global<const T>");
          }
          if (system.loop && system.effective.writesCache()) {
            throw std::logic_error("a listener of an event the loop \"" + system.name +
                                   "\" emits takes Resources<T>: all threads of the loop would change the cache; "
                                   "take it as Resources<const T>");
          }
        }

        // A system runs in the stage after the last earlier one it conflicts
        // with.
        std::vector<size_t> stageOf(m_systems.size(), 0);
        size_t count = 0;
        for (size_t i = 0; i < m_systems.size(); ++i) {
          for (size_t earlier = 0; earlier < i; ++earlier) {
            if (m_systems[i].effective.conflictsWith(m_systems[earlier].effective)) {
              stageOf[i] = std::max(stageOf[i], stageOf[earlier] + 1);
            }
          }
          count = std::max(count, stageOf[i] + 1);
        }
        m_stages.assign(count, {});
        for (size_t i = 0; i < m_systems.size(); ++i) {
          m_stages[stageOf[i]].push_back(i);
        }
        // The hooks run after the changes of a stage as well, all of them at
        // once.
        internal::SystemAccess hooks;
        if (world != nullptr) {
          for (const internal::HookAccess &hook: world->hookAccess()) {
            hooks.merge(*static_cast<const internal::SystemAccess *>(hook.access.get()));
          }
          if (hooks.modifiesGotten()) {
            throw std::logic_error("a hook edits a component that a hook also takes through a query: the hooks of "
                                   "the adds through Commands run at the same time");
          }
        }
        m_columns = hooks.columns;
        for (const System &system: m_systems) {
          for (const internal::ColumnMaker &maker: system.effective.columns) {
            if (std::ranges::find(m_columns, maker.index, &internal::ColumnMaker::index) == m_columns.end()) {
              m_columns.push_back(maker);
            }
          }
        }
        m_stagesWorld = world;
        m_stagesHookVersion = hookVersion;
        m_stagesValid = true;
      }
      return m_stages;
    }

    // What the hooks that apply runs make their parameters from: what they
    // record goes to the lists of the set.
    [[nodiscard]] internal::SystemContext hookContext(World &world, size_t set) {
      return {.world = world, .commands = m_hookLists[set], .caches = m_caches, .events = m_events, .name = HOOKS_NAME};
    }

    // What the hooks of the last apply recorded, which went to set 0, and
    // what the hooks of that record in turn, in rounds until none is left:
    // the hooks of one round record into the other set than the one applied.
    void runHookRounds(World &world) {
      size_t recorded = 0;
      for (size_t round = 1;
           std::ranges::any_of(m_hookLists[recorded], [](const internal::CommandList &list) { return !list.empty(); });
           ++round) {
        if (round > MAX_HOOK_ROUNDS) {
          throw std::logic_error("the hooks of components added and removed through Commands still recorded changes "
                                 "after 64 rounds");
        }
        m_stageLists.clear();
        for (internal::CommandList &list: m_hookLists[recorded]) {
          m_stageLists.push_back(&list);
        }
        const size_t next = 1 - recorded;
        const internal::SystemContext context = hookContext(world, next);
        world.advanceTick();
        m_applier.apply(world, m_stageLists, context);
        recorded = next;
      }
    }

    static constexpr size_t MAX_HOOK_ROUNDS = 64;
    inline static const std::string HOOKS_NAME = "the hooks of components added and removed through Commands";

    struct System {
      std::move_only_function<void(const internal::SystemContext &)> run;
      std::string name;
      // From its parameters.
      internal::SystemAccess access;
      // With what the listeners of the events it emits use, set by stages.
      internal::SystemAccess effective;
      // What the threads recorded through Commands, one list for every pool
      // thread and one for all others; empty for a system that does not take
      // Commands.
      std::vector<internal::CommandList> commands;
      // Added with addParallel: its listeners run on all threads of the loop.
      bool loop;
      // The tick of the world at the end of its last run, see
      // SystemContext::since.
      uint32_t lastRun = 0;
    };

    // What a system uses with what the listeners of its events use and the
    // hooks of the world for the components it edits, and those of the events
    // and components they go on to. Every event and component once, also
    // where they set each other off. Adds through Commands set off no hook
    // here: theirs run after the stage. foreign gets what the listeners and
    // hooks edit.
    [[nodiscard]] internal::SystemAccess effectiveAccess(const internal::SystemAccess &own, const World *world,
                                                         std::vector<size_t> &foreign) const {
      internal::SystemAccess effective = own;
      const auto take = [&](const internal::SystemAccess &other) {
        effective.merge(other);
        foreign.insert(foreign.end(), other.modified.begin(), other.modified.end());
      };
      std::vector<size_t> hooked;
      size_t emits = 0;
      size_t modified = 0;
      // merge appends the events and components of what it takes.
      while (emits < effective.emits.size() || modified < effective.modified.size()) {
        for (; emits < effective.emits.size(); ++emits) {
          const void *const tag = effective.emits[emits];
          for (const internal::EventListeners &event: m_events) {
            if (event.tag == tag) {
              for (const internal::Listener &listener: event.listeners) {
                take(listener.access);
              }
            }
          }
        }
        for (; modified < effective.modified.size(); ++modified) {
          const size_t component = effective.modified[modified];
          if (world == nullptr || std::ranges::find(hooked, component) != hooked.end()) {
            continue;
          }
          hooked.push_back(component);
          for (const internal::HookAccess &hook: world->hookAccess()) {
            if (hook.component == component) {
              take(*static_cast<const internal::SystemAccess *>(hook.access.get()));
            }
          }
        }
      }
      return effective;
    }

    template<typename F, typename EventParam, typename... Params>
    Scheduler &addListener(F &&listener, internal::List<EventParam, Params...>) {
      using Event = std::remove_cvref_t<EventParam>;
      static_assert(!std::is_reference_v<EventParam> || std::is_const_v<std::remove_reference_t<EventParam>>,
                    "a listener takes its event by value or const reference");
      static_assert(!internal::usesModifiedOtherwise<Params...>,
                    "a component the listener edits is not also named by its queries: a loop of the query would get "
                    "to an edited entity or not, depending on where it lies; read it in the function of modify");
      internal::SystemAccess access;
      (internal::SystemParamFor<Params>::declare(access), ...);
      if (sizeof...(Params) == 0) {
        access.exclusive = true;
      }

      internal::Listener made{
          .call =
              [function = std::decay_t<F>(std::forward<F>(listener))](const void *event,
                                                                      const internal::SystemContext &context) {
                function(*static_cast<const Event *>(event), internal::SystemParamFor<Params>::fetch(context)...);
              },
          .access = std::move(access),
      };
      const void *const tag = &internal::eventTag<Event>;
      auto found = std::ranges::find(m_events, tag, &internal::EventListeners::tag);
      if (found == m_events.end()) {
        m_events.push_back({.tag = tag, .listeners = {}});
        found = std::prev(m_events.end());
      }
      found->listeners.push_back(std::move(made));
      m_stagesValid = false;
      return *this;
    }

    // The columns the systems and their listeners use that the world does
    // not have yet, made before the first system, so that none is made while
    // the threads of a loop wait for it. One after the other: two columns
    // took 9 µs that way and 20 to 22 µs on two threads, as making one
    // reserves addresses, which the OS does for one thread of a process at a
    // time, and the pool threads had to be woken up first.
    void makeColumns(World &world) {
      for (const internal::ColumnMaker &maker: m_columns) {
        if (!maker.made(world)) {
          maker.make(world);
        }
      }
    }

    // Drops what the systems of a stage recorded through Commands.
    void clearCommands(const std::vector<size_t> &stage) noexcept {
      for (const size_t index: stage) {
        for (internal::CommandList &list: m_systems[index].commands) {
          list.clear();
        }
      }
    }

    // With LOOP, the first parameter is the query of the loop of addParallel.
    template<bool LOOP = false, typename F, typename... Params>
    Scheduler &addSystem(F &&system, const std::string &name, internal::List<Params...>) {
      static_assert(!internal::editsQueried<LOOP, Params...>,
                    "a component the system edits is not also named by its queries: a loop of the query would get "
                    "to an edited entity or not, depending on where it lies; read it in the function of modify");
      internal::SystemAccess access;
      declare<LOOP, Params...>(access);
      if (sizeof...(Params) == 0) {
        access.exclusive = true;
      }

      m_systems.push_back(System{
          .run =
              [function = std::decay_t<F>(std::forward<F>(system))](const internal::SystemContext &context) mutable {
                // For the hooks of what it adds through the world.
                const internal::HookScope scope{&context};
                function(internal::SystemParamFor<Params>::fetch(context)...);
              },
          .name = std::move(name),
          .access = std::move(access),
          .effective = {},
          .commands = {},
          .loop = LOOP,
      });
      m_stagesValid = false;
      return *this;
    }

    template<bool LOOP, typename First, typename... Rest>
    static void declare(internal::SystemAccess &access) {
      internal::SystemParamFor<First>::declare(access);
      if constexpr (LOOP) {
        // Not for modifiesGotten: what the loop goes through, the function
        // only edits on its own entity.
        access.gotten.clear();
      }
      (internal::SystemParamFor<Rest>::declare(access), ...);
    }
    template<bool LOOP>
    static void declare(internal::SystemAccess &) {}

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
      static_assert(!(internal::writesGlobal<std::remove_cvref_t<Extras>> || ...),
                    "the function of a loop takes a global value as Global<const T>: all threads of the loop would "
                    "write the one value");
      static_assert(!(internal::writesCache<std::remove_cvref_t<Extras>> || ...),
                    "the function of a loop takes a cache as Resources<const T>: all threads of the loop would change "
                    "the cache");
      using Function = std::decay_t<F>;
      using Signature = internal::LoopSignature<typename internal::SystemSignature<Function>::Params>;
      return addSystem<true>(
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
    std::vector<internal::ProvidedCache> m_caches;
    // Per type of event; Emit points into it while systems run.
    std::vector<internal::EventListeners> m_events;
    // Of all systems and listeners, set by stages.
    std::vector<internal::ColumnMaker> m_columns;
    // The world and its hooks the stages were made for.
    const World *m_stagesWorld = nullptr;
    uint64_t m_stagesHookVersion = 0;
    // What the hooks run after the changes of a stage record, two sets, see
    // runHookRounds.
    std::array<std::vector<internal::CommandList>, 2> m_hookLists;
  };

} // namespace sc::ecs

namespace sc::ecs::internal {

  // Whether a hook may take the parameter: not the world, which would make
  // every system that sets it off run alone.
  template<typename Param>
  inline constexpr bool hookParam = !std::is_same_v<Param, World &>;

  // A hook of World::onAdd or World::onRemove for the component, called with
  // the entity, the component and its parameters, made from hookContext.
  template<typename Component, typename Params>
  struct HookOf;

  template<typename Component, typename EntityParam, typename ValueParam, typename... Params>
  struct HookOf<Component, List<EntityParam, ValueParam, Params...>> {
    static_assert(std::is_same_v<std::remove_cvref_t<EntityParam>, Entity>, "a hook takes the entity first");
    static_assert(std::is_lvalue_reference_v<ValueParam> &&
                      std::is_same_v<std::remove_cvref_t<ValueParam>, std::remove_const_t<Component>>,
                  "a hook takes the component second, by reference");
    static_assert((hookParam<Params> && ...), "a hook does not take the world");
    static_assert(!(writesGlobal<std::remove_cvref_t<Params>> || ...),
                  "a hook runs on several threads at once: it takes a global value as Global<const T>");
    static_assert(!(writesCache<std::remove_cvref_t<Params>> || ...),
                  "a hook runs on several threads at once: it takes a cache as Resources<const T>");
    static_assert(!usesModifiedOtherwise<Params...>,
                  "a component the hook edits is not also named by its queries, as for a system");

    static SystemAccess access() {
      SystemAccess access;
      (SystemParamFor<Params>::declare(access), ...);
      return access;
    }

    template<typename F>
    static auto make(World &world, F &&hook) {
      return [function = std::decay_t<F>(std::forward<F>(hook)), &world](Entity entity, Component &value) {
        if (const SystemContext *const context = hookContext; context != nullptr) {
          function(entity, value, SystemParamFor<Params>::fetch(*context)...);
          return;
        }
        // Outside a system: no lists for Commands, no listeners for Emit.
        static const std::string NAME = "a hook outside a system";
        const SystemContext outside{.world = world, .commands = {}, .caches = {}, .events = {}, .name = NAME};
        function(entity, value, SystemParamFor<Params>::fetch(outside)...);
      };
    }
  };

} // namespace sc::ecs::internal

namespace sc::ecs {

  template<typename Component, typename F>
  World &World::onAdd(F &&hook) {
    static_assert(std::is_same_v<Component, std::remove_cvref_t<Component>>, "onAdd names the type of the component");
    using Hook = internal::HookOf<Component, typename internal::SystemSignature<std::decay_t<F>>::Params>;
    column<Component>().addHook(Hook::make(*this, std::forward<F>(hook)));
    m_hookAccess.push_back({.component = internal::componentIndex<Component>(),
                            .access = std::make_shared<const internal::SystemAccess>(Hook::access())});
    ++m_hookVersion;
    return *this;
  }

  template<typename Component, typename F>
  World &World::onRemove(F &&hook) {
    static_assert(std::is_same_v<Component, std::remove_cvref_t<Component>>,
                  "onRemove names the type of the component");
    using Hook = internal::HookOf<Component, typename internal::SystemSignature<std::decay_t<F>>::Params>;
    ColumnOf<Component> &column = this->column<Component>();
    column.addRemoveHook(Hook::make(*this, std::forward<F>(hook)));
    if (std::ranges::find(m_removeHooked, &column) == m_removeHooked.end()) {
      m_removeHooked.push_back(&column);
    }
    m_hookAccess.push_back({.component = internal::componentIndex<Component>(),
                            .access = std::make_shared<const internal::SystemAccess>(Hook::access())});
    ++m_hookVersion;
    return *this;
  }

} // namespace sc::ecs
