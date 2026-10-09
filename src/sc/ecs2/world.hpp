#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <utility>

#include "column.hpp"
#include "entity.hpp"
#include "sc/threading/threading.hpp"

namespace sc::ecs2 {

  // Names components the entities of a loop must not have:
  // world.each<Position, Exclude<Frozen>>(...).
  template <typename... Components> struct Exclude {};

} // namespace sc::ecs2

namespace sc::ecs2::internal {

  inline constexpr size_t MAX_COMPONENT_TYPES = 256;

  inline std::atomic<size_t> nextComponentIndex{0};

  // A number for every component type of the program, the same in all worlds.
  template <typename Component> size_t componentIndex() noexcept {
    static const size_t index = nextComponentIndex.fetch_add(1, std::memory_order_relaxed);
    return index;
  }

  template <typename... Types> struct List {};

  // Sorts the parameters of a loop into the components to get and the
  // excluded ones.
  template <typename Get, typename Excluded, typename... Terms> struct Split;

  template <typename... Get, typename... Excluded> struct Split<List<Get...>, List<Excluded...>> {
    using GetList = List<Get...>;
    using ExcludedList = List<Excluded...>;
  };

  template <typename... Get, typename... Excluded, typename Term, typename... Rest>
  struct Split<List<Get...>, List<Excluded...>, Term, Rest...> : Split<List<Get..., Term>, List<Excluded...>, Rest...> {
  };

  template <typename... Get, typename... Excluded, typename... More, typename... Rest>
  struct Split<List<Get...>, List<Excluded...>, Exclude<More...>, Rest...>
      : Split<List<Get...>, List<Excluded..., More...>, Rest...> {};

  // The component at a slot of a page's values, nothing for a component
  // without members.
  template <typename Values> auto componentOn(Values values, uint32_t slot) {
    if constexpr (std::is_null_pointer_v<Values>) {
      return std::tuple<>{};
    } else {
      return std::forward_as_tuple(values[slot]);
    }
  }

  // Whether a function takes the entity before the components of a tuple.
  template <typename Function, typename Components> inline constexpr bool takesEntity = false;
  template <typename Function, typename... Components>
  inline constexpr bool takesEntity<Function, std::tuple<Components...>> =
      std::is_invocable_v<Function &, Entity, Components...>;

} // namespace sc::ecs2::internal

namespace sc::ecs2 {

  // Entities and their components.
  //
  // A component is stored at the index of its entity, so all components of an
  // entity are at the same place of their columns and a loop over several
  // components needs neither a lookup nor components kept in a common order.
  //
  // create and valid are safe from the threads of the pool at any time. A
  // component type is either read by any number of threads or written by one.
  // Adding and removing components is also safe for several threads as long as
  // every thread keeps to entities of other pages, which is what the functions
  // of parallelEach do. destroy must have nothing running next to it.
  class World {
  public:
    template <typename Component> using ColumnOf = internal::Column<std::remove_const_t<Component>>;

    World() = default;

    ~World() {
      const size_t count = m_columnCount.load(std::memory_order_relaxed);
      for (size_t i = 0; i < count; ++i) {
        delete m_columnList[i];
      }
    }

    World(const World &) = delete;
    World &operator=(const World &) = delete;

    [[nodiscard]] Entity create() { return m_entities.create(); }

    // An entity that is unlike most others, see Rare. It is an entity like any
    // other, only with an index from the other end.
    [[nodiscard]] Entity create(Rare) { return m_entities.create(rare); }

    // An entity in a page that was made after the last sealLocal and that only
    // the calling thread creates entities in. Several threads may create
    // entities this way and give them components right away, next to threads
    // that go through the entities there were at the last sealLocal.
    [[nodiscard]] Entity createLocal() { return m_entities.createLocal(); }

    // Ends the pages of createLocal, see there. To be called before threads
    // go through entities and create others with createLocal, which the
    // Scheduler does for every stage. Must have nothing running next to it.
    void sealLocal() { m_entities.seal(); }

    [[nodiscard]] bool valid(Entity entity) const noexcept { return m_entities.valid(entity); }

    // Removes the entity with all of its components. False if it does not
    // exist (any more).
    bool destroy(Entity entity) {
      if (!valid(entity)) {
        return false;
      }
      eraseAll(entity);
      m_entities.destroy(entity);
      return true;
    }

    // The first half of destroy: removes the components and leaves the entity.
    // Safe next to threads that keep to entities of other pages.
    void eraseAll(Entity entity) noexcept {
      const size_t count = m_columnCount.load(std::memory_order_acquire);
      for (size_t i = 0; i < count; ++i) {
        m_columnList[i]->erase(indexOf(entity));
      }
    }

    // The second half: the entity must exist and have no components.
    void release(Entity entity) { m_entities.destroy(entity); }

    template <typename Component, typename... Args> decltype(auto) emplace(Entity entity, Args &&...args) {
      return column<Component>().emplace(indexOf(entity), std::forward<Args>(args)...);
    }

    template <typename Component> bool erase(Entity entity) { return column<Component>().erase(indexOf(entity)); }

    template <typename Component> [[nodiscard]] bool contains(Entity entity) {
      return column<Component>().contains(indexOf(entity));
    }

    // The entity must have the component.
    template <typename Component> [[nodiscard]] Component &get(Entity entity) {
      return column<Component>().get(indexOf(entity));
    }

    // The column of a component, which is made on first use.
    template <typename Component> [[nodiscard]] ColumnOf<Component> &column() {
      using Type = std::remove_const_t<Component>;
      std::atomic<internal::ColumnBase *> &slot = m_columns[internal::componentIndex<Type>()];
      if (internal::ColumnBase *found = slot.load(std::memory_order_acquire)) [[likely]] {
        return static_cast<internal::Column<Type> &>(*found);
      }
      return makeColumn<Type>(slot);
    }

    // Calls the function with the components of every entity that has all of
    // them. A const component is only read; the entity comes first if the
    // function takes one:
    //
    //   world.each<Position, const Velocity>([](Position &pos, const Velocity &vel) { ... });
    //   world.each<const Health, Exclude<Frozen>>([](Entity entity, const Health &health) { ... });
    //
    // The function may add and remove components of the entity it is called
    // with, as long as they are none of those the loop names, and must leave
    // other entities as they are.
    template <typename... Terms, typename Function> void each(Function &&function) {
      using Lists = internal::Split<internal::List<>, internal::List<>, Terms...>;
      const PageRange pages = pageRange();
      eachInRange(typename Lists::GetList{}, typename Lists::ExcludedList{}, pages, 0, pages.count(), function);
    }

    // The same on the calling thread and on every pool thread that has nothing
    // else to do. Each thread gets pages of its own, so the function may add
    // and remove components of its entity here as well.
    template <typename... Terms, typename Function> void parallelEach(Function &&function, size_t grain = 1) {
      using Lists = internal::Split<internal::List<>, internal::List<>, Terms...>;
      const PageRange pages = pageRange();
      // A thread for every so many entities: a page with few entities in it
      // is not worth waking one up.
      const size_t helpers =
          countUpTo(typename Lists::GetList{}, pages, threading::thread_slots() * ENTITIES_PER_THREAD) /
          ENTITIES_PER_THREAD;
      threading::parallelFor(pages.count(), grain, helpers, [&](size_t first, size_t last) {
        eachInRange(typename Lists::GetList{}, typename Lists::ExcludedList{}, pages, static_cast<uint32_t>(first),
                    static_cast<uint32_t>(last), function);
      });
    }

    // No common entity is in a page with this number or a higher one. The
    // rare ones are in the last pages there are.
    [[nodiscard]] uint32_t pageLimit() const noexcept { return m_entities.pages().common; }

  private:
    // How many entities a loop needs before another thread helps with it.
    static constexpr size_t ENTITIES_PER_THREAD = 1024;

    // The pages that may have entities, as one row: the common ones first and
    // then the rare ones, which are the last pages there are.
    struct PageRange {
      uint32_t common = 0;
      uint32_t rare = 0;

      [[nodiscard]] uint32_t count() const noexcept { return common + rare; }
      // The number of the page at a place in the row.
      [[nodiscard]] uint32_t page(uint32_t place) const noexcept {
        return place < common ? place : rareBegin() + (place - common);
      }
      [[nodiscard]] uint32_t rareBegin() const noexcept { return static_cast<uint32_t>(internal::PAGE_COUNT) - rare; }
    };

    [[nodiscard]] PageRange pageRange() const noexcept {
      const auto pages = m_entities.pages();
      return {.common = pages.common, .rare = pages.rare};
    }

    // eachInPages for a part of the row of pages.
    template <typename Get, typename Excluded, typename Function>
    void eachInRange(Get get, Excluded excluded, PageRange pages, uint32_t first, uint32_t last, Function &function) {
      if (first < pages.common) {
        eachInPages(get, excluded, first, std::min(last, pages.common), function);
      }
      if (last > pages.common) {
        eachInPages(get, excluded, pages.page(std::max(first, pages.common)), pages.rareBegin() + (last - pages.common),
                    function);
      }
    }

    // How many entities of the pages have all of the components, at most.
    // Stops counting at the limit.
    template <typename... Get> size_t countUpTo(internal::List<Get...>, PageRange pages, size_t limit) {
      const std::tuple columns{&column<Get>()...};
      size_t count = 0;
      for (uint32_t place = 0; place < pages.count() && count < limit; ++place) {
        const uint32_t number = pages.page(place);
        count += std::apply(
            [number](const auto *...each) {
              return std::min({[&]() -> uint32_t { return each->page(number).count; }()...});
            },
            columns);
      }
      return count;
    }

    template <typename Type> internal::Column<Type> &makeColumn(std::atomic<internal::ColumnBase *> &slot) {
      std::scoped_lock lock{m_columnMutex};
      if (internal::ColumnBase *found = slot.load(std::memory_order_relaxed)) {
        return static_cast<internal::Column<Type> &>(*found);
      }
      auto *made = new internal::Column<Type>;
      const size_t count = m_columnCount.load(std::memory_order_relaxed);
      m_columnList[count] = made;
      m_columnCount.store(count + 1, std::memory_order_release);
      slot.store(made, std::memory_order_release);
      return *made;
    }

    // The values of a page as the function gets them: const for a component
    // that is only read, null for one without members.
    template <typename Component> static auto valuesOf(const typename ColumnOf<Component>::Page &page) noexcept {
      if constexpr (!ColumnOf<Component>::HAS_VALUE) {
        return nullptr;
      } else {
        return static_cast<Component *>(page.values);
      }
    }

    template <typename... Get, typename... Excluded, typename Function>
    void eachInPages(internal::List<Get...>, internal::List<Excluded...>, uint32_t firstPage, uint32_t lastPage,
                     Function &function) {
      static_assert(sizeof...(Get) != 0, "a loop needs a component to go through");
      [&]<size_t... G, size_t... X>(std::index_sequence<G...>, std::index_sequence<X...>) {
        const std::tuple columns{&column<Get>()...};
        const std::tuple excluded{&column<Excluded>()...};

        for (uint32_t number = firstPage; number < lastPage; ++number) {
          const std::tuple pages{std::get<G>(columns)->page(number)...};
          if ((!std::get<G>(pages) || ...)) {
            // No entity of this page has all of them.
            continue;
          }
          const std::tuple without{std::get<X>(excluded)->page(number)...};
          const std::tuple values{valuesOf<Get>(std::get<G>(pages))...};
          const uint32_t base = number << PAGE_BITS;

          using Components = decltype(std::tuple_cat(internal::componentOn(std::get<G>(values), uint32_t{0})...));
          constexpr bool WITH_ENTITY = internal::takesEntity<Function, Components>;
          [[maybe_unused]] const uint32_t *generations = WITH_ENTITY ? m_entities.generation(base) : nullptr;

          const auto call = [&](uint32_t slot) {
            if constexpr (WITH_ENTITY) {
              std::apply(function, std::tuple_cat(std::tuple{makeEntity(base + slot, generations[slot])},
                                                  internal::componentOn(std::get<G>(values), slot)...));
            } else {
              std::apply(function, std::tuple_cat(internal::componentOn(std::get<G>(values), slot)...));
            }
          };

          // Every entity of the page: one loop the compiler turns into SIMD
          // code.
          if (((std::get<G>(pages).count == PAGE_SIZE) && ...) && (!std::get<X>(without) && ...)) {
            for (uint32_t slot = 0; slot < PAGE_SIZE; ++slot) {
              call(slot);
            }
            continue;
          }

          for (uint32_t word = 0; word < internal::PAGE_WORDS; ++word) {
            uint64_t mask = (std::get<G>(pages).mask[word] & ...);
            ((mask &= std::get<X>(without) ? ~std::get<X>(without).mask[word] : ~uint64_t{0}), ...);
            const uint32_t first = word * 64;
            if (mask == ~uint64_t{0}) {
              for (uint32_t slot = first; slot < first + 64; ++slot) {
                call(slot);
              }
            } else {
              for (; mask != 0; mask &= mask - 1) {
                call(first + static_cast<uint32_t>(std::countr_zero(mask)));
              }
            }
          }
        }
      }(std::index_sequence_for<Get...>{}, std::index_sequence_for<Excluded...>{});
    }

    internal::EntityAllocator m_entities;

    // The column of every component type used with this world, at
    // internal::componentIndex. An entry is written once, under the mutex, and
    // read without a lock by all threads.
    std::array<std::atomic<internal::ColumnBase *>, internal::MAX_COMPONENT_TYPES> m_columns{};
    // The same columns in the order they were made, for what goes through all.
    std::array<internal::ColumnBase *, internal::MAX_COMPONENT_TYPES> m_columnList{};
    std::atomic<size_t> m_columnCount{0};
    std::mutex m_columnMutex;
  };

} // namespace sc::ecs2
