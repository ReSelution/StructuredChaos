#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include <mimalloc.h>

#include "sc/memory/virtual_memory.hpp"
#include "sc/threading/threading.hpp"
#include "sc/util/macros.hpp"

namespace sc::ecs {

  // An entity: the index its components are stored at and, above it, the
  // generation of that index, which tells the entity apart from earlier ones
  // that had the same index.
  //
  // The bits are split up here and nowhere else. Where in the index an entity
  // lies says which area it was made in, see EntityAllocator.
  enum class Entity : uint64_t {};

  inline constexpr unsigned INDEX_BITS = 32;

  // Components are stored in pages of this many entities. Threads that work
  // on different pages share nothing.
  inline constexpr unsigned PAGE_BITS = 12;
  inline constexpr uint32_t PAGE_SIZE = uint32_t{1} << PAGE_BITS;

  // No entity has this index or a higher one, so there are never more
  // entities than this at a time. It is what every column reserves addresses
  // for. All of the index but its last page: the index of null lies in it
  // and above every other one, and the end of a page is still a uint32_t.
  inline constexpr uint32_t INDEX_LIMIT = ~uint32_t{0} - PAGE_SIZE + 1;

  [[nodiscard]] constexpr uint32_t indexOf(Entity entity) noexcept {
    return static_cast<uint32_t>(static_cast<uint64_t>(entity));
  }

  [[nodiscard]] constexpr uint32_t generationOf(Entity entity) noexcept {
    return static_cast<uint32_t>(static_cast<uint64_t>(entity) >> INDEX_BITS);
  }

  [[nodiscard]] constexpr Entity makeEntity(uint32_t index, uint32_t generation) noexcept {
    return Entity{(uint64_t{generation} << INDEX_BITS) | index};
  }

  // No entity has this value.
  inline constexpr Entity null{~uint64_t{0}};

  // Says that an entity is unlike most others: world.create(rare). Such
  // entities get their indices from an area of their own, where they are
  // among themselves. A component that only they have then needs memory for
  // their few pages and not for pages all over the world.
  //
  // Rare is one fixed area without a set: all rare entities share it,
  // whatever components they get, so players, bosses and cameras made this
  // way lie mixed in the same pages. When the components are known at
  // create, a set does better: world.create<Player>() or
  // world.create(Player{...}) gives every set an area of its own. Rare is
  // for the few entities whose components are not known yet when they are
  // made.
  struct Rare {};
  inline constexpr Rare rare{};

} // namespace sc::ecs

namespace sc::ecs::internal {

  // Hands out entities and knows which ones exist.
  //
  // The indices come a page at a time from areas: every set of components
  // the world makes entities with gets an area of its own, so that entities
  // with the same components lie together in the same pages. Area k starts
  // at the page whose number is k with its PAGE_NUMBER_BITS bits in reverse
  // order: 0, half of all pages, a quarter, three quarters, an eighth and so
  // on. Every new area halves the largest gap left, so the areas lie as far
  // apart as their count allows, without a fixed distance (the van der
  // Corput sequence). An area that runs into a page of another one goes on
  // with the next free page behind it, as the block groups of ext4 do.
  // Area COMMON_AREA is that of entities made without components, RARE_AREA
  // that of world.create(rare).
  //
  // Every thread has a page of its own per area to take new indices from,
  // and takes another one when that is used up. Indices that destroy gives
  // back go to the area of their page.
  //
  // create and valid are safe from any thread at any time. The threads of the
  // pool and the thread that set it up need no lock for it, every other thread
  // takes turns with the others of its kind for a new index. destroy must have
  // nothing running next to it.
  class EntityAllocator {
  public:
    static constexpr size_t MAX_THREADS = 128;

  private:
    // The generations of this many indices are kept in one piece.
    static constexpr unsigned BLOCK_BITS = 16;
    static constexpr uint32_t BLOCK_SIZE = uint32_t{1} << BLOCK_BITS;
    // Also for the indices from INDEX_LIMIT on, which share a block with
    // those below it.
    static constexpr size_t BLOCK_COUNT = size_t{1} << (INDEX_BITS - BLOCK_BITS);
    // The bits of a page number, and how many pages may be handed out: all
    // but the last one, see INDEX_LIMIT.
    static constexpr unsigned PAGE_NUMBER_BITS = INDEX_BITS - PAGE_BITS;
    static constexpr uint32_t PAGE_COUNT = INDEX_LIMIT >> PAGE_BITS;

    // The indices a thread hands out next, which nobody else has.
    struct alignas(64) Share {
      uint32_t next = 0;
      uint32_t end = 0;
    };

    // The indices destroy gave back, to be handed out again.
    struct FreeList {
      // How many of the indices are left to hand out, from the back. Below
      // zero when several threads went for the last one.
      alignas(64) std::atomic<int64_t> left{0};
      // Only changed by give, so take reads it without a lock.
      std::vector<uint32_t> indices;

      // False if there is none.
      [[nodiscard]] bool take(uint32_t &index) noexcept {
        // Read first: once nothing is left, no thread writes this any more.
        if (left.load(std::memory_order_acquire) <= 0) {
          return false;
        }
        const int64_t before = left.fetch_sub(1, std::memory_order_acq_rel);
        if (before <= 0) {
          return false;
        }
        index = indices[static_cast<size_t>(before - 1)];
        return true;
      }

      // take for up to count indices with one fetch_sub: those of the span,
      // in the order destroy gave them back. Empty if there is none.
      [[nodiscard]] std::span<const uint32_t> takeMany(size_t count) noexcept {
        if (count == 0 || left.load(std::memory_order_acquire) <= 0) {
          return {};
        }
        const int64_t before = left.fetch_sub(static_cast<int64_t>(count), std::memory_order_acq_rel);
        if (before <= 0) {
          return {};
        }
        const auto taken = static_cast<size_t>(std::min(before, static_cast<int64_t>(count)));
        return {indices.data() + (static_cast<size_t>(before) - taken), taken};
      }

      // Must have nothing running next to it.
      void give(uint32_t index) {
        open();
        add(index);
        close();
      }

      // give for many indices: open, add each, close. Must have nothing
      // running next to it from open to close.
      void open() {
        // What take took since the last time.
        indices.resize(static_cast<size_t>(std::max<int64_t>(left.load(std::memory_order_relaxed), 0)));
      }
      void add(uint32_t index) { indices.push_back(index); }
      void close() { left.store(static_cast<int64_t>(indices.size()), std::memory_order_release); }

      // How many indices are left to hand out.
      [[nodiscard]] size_t size() const noexcept {
        return static_cast<size_t>(std::max<int64_t>(left.load(std::memory_order_acquire), 0));
      }
    };

    // What an area has: where it looks for its next page, the indices
    // destroy gave back and the page of every thread.
    struct Area {
      explicit Area(uint32_t start) : cursor(start) {}

      // The page to look at first for the next one. Only changed under
      // m_pageMutex.
      uint32_t cursor;
      FreeList free;
      // The last one for the threads without one of their own, see withShare.
      std::array<Share, MAX_THREADS + 1> shares{};
    };

  public:
    // Which area the entities made without components and the rare ones go
    // to. The world numbers the areas of its sets of components from
    // FIRST_AREA on.
    static constexpr uint32_t COMMON_AREA = 0;
    static constexpr uint32_t RARE_AREA = 1;
    static constexpr uint32_t FIRST_AREA = 2;
    // A page knows its area in 16 bits, and 0 is for no area.
    static constexpr uint32_t AREA_LIMIT = 0xFFFF;

  private:
    // Where the tables lie in the range of the allocator, see m_reserved.
    // Each one a whole number of OS pages, so that each starts at one.
    static constexpr size_t BLOCKS_BYTES = BLOCK_COUNT * sizeof(std::atomic<uint32_t *>);
    static constexpr size_t OWNERS_BYTES = (size_t{1} << PAGE_NUMBER_BITS) * sizeof(std::atomic<uint16_t>);
    static constexpr size_t ORDER_BYTES = (size_t{1} << PAGE_NUMBER_BITS) * sizeof(uint32_t);
    static constexpr size_t AREAS_BYTES = size_t{AREA_LIMIT + 1} * sizeof(std::atomic<Area *>);
    static constexpr size_t OWNERS_AT = BLOCKS_BYTES;
    static constexpr size_t ORDER_AT = OWNERS_AT + OWNERS_BYTES;
    static constexpr size_t AREAS_AT = ORDER_AT + ORDER_BYTES;
    static constexpr size_t RESERVED_BYTES = AREAS_AT + AREAS_BYTES;
    static_assert(OWNERS_AT % memory::OS_PAGE_SIZE == 0 && ORDER_AT % memory::OS_PAGE_SIZE == 0 &&
                  AREAS_AT % memory::OS_PAGE_SIZE == 0);

  public:
    // The tables get memory with their first use, all zero: 7 MiB of
    // addresses in one range, of which most worlds touch a page or two of
    // each.
    //
    // With pageBits below PAGE_NUMBER_BITS only the first 2^pageBits pages
    // are handed out, for tests that use them all up; every area starts
    // where it would, scaled down to them.
    explicit EntityAllocator(unsigned pageBits = PAGE_NUMBER_BITS) :
        m_pageLimit(pageBits >= PAGE_NUMBER_BITS ? PAGE_COUNT : uint32_t{1} << pageBits),
        m_startShift(pageBits >= PAGE_NUMBER_BITS ? 0 : PAGE_NUMBER_BITS - pageBits),
        m_reserved(static_cast<std::byte *>(memory::reserve(RESERVED_BYTES))) {
      try {
        memory::commit(m_reserved, RESERVED_BYTES);
      } catch (...) {
        memory::release(m_reserved, RESERVED_BYTES);
        throw;
      }
      areaTable()[COMMON_AREA].store(&m_common, std::memory_order_relaxed);
      m_areaCount.store(COMMON_AREA + 1, std::memory_order_relaxed);
    }

    ~EntityAllocator() {
      // Only the blocks of pages handed out have memory, a block may have
      // several of them.
      const uint32_t pages = pageCount();
      for (uint32_t place = 0; place < pages; ++place) {
        mi_free(blocks()[pageAt(place) >> (BLOCK_BITS - PAGE_BITS)].exchange(nullptr, std::memory_order_relaxed));
      }
      const uint32_t areas = m_areaCount.load(std::memory_order_relaxed);
      // m_common is the allocator's own.
      for (uint32_t area = COMMON_AREA + 1; area < areas; ++area) {
        delete areaTable()[area].load(std::memory_order_relaxed);
      }
      memory::release(m_reserved, RESERVED_BYTES);
    }

    EntityAllocator(const EntityAllocator &) = delete;
    EntityAllocator &operator=(const EntityAllocator &) = delete;

    // An index of the area that was given back by destroy, or else the next
    // one of the calling thread there. Throws std::length_error if all pages
    // are taken, std::out_of_range for an area not below AREA_LIMIT.
    [[nodiscard]] Entity create(uint32_t area) { return create(this->area(area), area); }

    // The same in COMMON_AREA, which the allocator holds itself: through the
    // table of areas, 1M creates took 8.4 instead of 7.7 ms.
    [[nodiscard]] Entity create() { return create(m_common, COMMON_AREA); }

    // An entity for every place of the span, as create makes them. Calls the
    // function with every run of indices it handed out, as (first index,
    // count): what destroy gave back in the rows it lies in, the rest in rows
    // as long as the pages of the thread allow. For a thread outside the pool
    // the function is called under the lock of withShare. If the function
    // throws, every entity of the span is made all the same, the function is
    // not called again, and the exception is thrown on.
    //
    // The indices given back are taken all at once, and every row of them
    // goes to the function in one call: one by one, a column made every one
    // of them on its own (count, mask and summary per entity), 14.5 instead
    // of 1.3 ns per entity with two components for indices that destroy had
    // given back in a row (BM_CreateEntitiesInBulk of ecs_benchmark, gcc);
    // in rows 2.1 ns at 1024 entities and 6.3 at 262144.
    template<typename Function>
    void create(std::span<Entity> entities, uint32_t area, Function &&run) {
      create(entities, this->area(area), area, run);
    }

    template<typename Function>
    void create(std::span<Entity> entities, Function &&run) {
      create(entities, m_common, COMMON_AREA, run);
    }

    // An index among those of the rare entities.
    [[nodiscard]] Entity create(Rare) { return create(RARE_AREA); }

  private:
    [[nodiscard]] Entity create(Area &in, uint32_t area) {
      if (uint32_t index = 0; in.free.take(index)) {
        return makeEntity(index, *generation(index));
      }

      return withShare(in, [&](Share &share) {
        if (share.next == share.end) [[unlikely]] {
          share.next = makePage(in, area);
          share.end = share.next + PAGE_SIZE;
        }
        // Never handed out before.
        return makeEntity(share.next++, 0);
      });
    }

    template<typename Function>
    void create(std::span<Entity> entities, Area &in, uint32_t area, Function &run) {
      // Once the function threw, the entities are made all the same, without
      // it, and the exception goes on afterwards: stopped there, the indices
      // taken from the free list after it were lost, and the new ones of the
      // run it threw in were handed out once more by the next create.
      std::exception_ptr failed;
      const auto give = [&](uint32_t first, uint32_t count) {
        if (failed) {
          return;
        }
        try {
          run(first, count);
        } catch (...) {
          failed = std::current_exception();
        }
      };
      size_t made = 0;
      const std::span<const uint32_t> reused = in.free.takeMany(entities.size());
      for (size_t i = 0; i < reused.size();) {
        const uint32_t first = reused[i];
        uint32_t count = 1;
        while (i + count < reused.size() && reused[i + count] == first + count) {
          ++count;
        }
        for (uint32_t k = 0; k < count; ++k) {
          entities[made + k] = makeEntity(first + k, *generation(first + k));
        }
        give(first, count);
        made += count;
        i += count;
      }

      withShare(in, [&](Share &share) {
        while (made < entities.size()) {
          if (share.next == share.end) {
            share.next = makePage(in, area);
            share.end = share.next + PAGE_SIZE;
          }
          const auto count = static_cast<uint32_t>(std::min<size_t>(entities.size() - made, share.end - share.next));
          for (uint32_t i = 0; i < count; ++i) {
            entities[made + i] = makeEntity(share.next + i, 0);
          }
          give(share.next, count);
          share.next += count;
          made += count;
        }
      });
      if (failed) {
        std::rethrow_exception(failed);
      }
    }

  public:
    [[nodiscard]] bool valid(Entity entity) const noexcept {
      const uint32_t index = indexOf(entity);
      // Also for an index from INDEX_LIMIT on: its page is never handed out.
      if (owners()[index >> PAGE_BITS].load(std::memory_order_acquire) == 0) {
        return false;
      }
      const uint32_t *current = generation(index);
      return current != nullptr && *current == generationOf(entity);
    }

    // The entity must exist. Its index is handed out again with another
    // generation, so that the entity stays invalid.
    void destroy(Entity entity) {
      const uint32_t index = indexOf(entity);
      ++*generation(index);
      // Back to where it came from.
      areaOf(index).free.give(index);
    }

    // destroy for every entity that exists: one given twice or no longer
    // there is left out. Calls the function with the index of each one it
    // destroys and returns how many those were. The indices go back to their
    // lists in one step each.
    //
    // Inlined into the caller, so that the function is one of its locals:
    // called with it from World::destroyQuietly, Clang kept the pointer to it
    // on the stack and loaded the world through it again for every entity.
    // Creating 100K entities with 2 components at once and destroying them
    // at once took 2.23 instead of 2.77M cycles (535 instead of 658 µs); GCC
    // inlined it: 2.06M either way.
    template<typename Function>
    SC_FORCE_INLINE size_t destroyMany(std::span<const Entity> entities, Function &&function) {
      openAll();
      size_t destroyed = 0;
      LastList last;
      for (const Entity entity: entities) {
        if (!valid(entity)) {
          continue;
        }
        const uint32_t index = indexOf(entity);
        ++*generation(index);
        listOf(index, last).add(index);
        function(index);
        ++destroyed;
      }
      closeAll();
      return destroyed;
    }

    // How many entities exist: every index of the pages handed out to the
    // threads, less those the threads have not handed out yet and those
    // destroy gave back. Worked out, not counted by create and destroy: a
    // count would be one more locked instruction per create, on a cache line
    // that every creating thread writes. Reads the Share of every thread in
    // every area, 129 cache lines each. Nothing may create or destroy next to
    // it, as next to destroy: the shares are the threads' own and read
    // without a lock.
    [[nodiscard]] size_t alive() const noexcept {
      size_t count = size_t{pageCount()} << PAGE_BITS;
      const uint32_t areas = m_areaCount.load(std::memory_order_acquire);
      for (uint32_t area = 0; area < areas; ++area) {
        const Area *const in = areaTable()[area].load(std::memory_order_acquire);
        if (in == nullptr) {
          continue;
        }
        for (const Share &share: in->shares) {
          count -= size_t{share.end - share.next};
        }
        count -= in->free.size();
      }
      return count;
    }

    // destroyMany in two halves, for threads that take a page of entities
    // each: kill ends the entity if it exists, without giving its index back,
    // which giveMany does afterwards for all of them on one thread. kill only
    // touches the generation of the index, so threads may kill entities of
    // different pages at the same time, but nothing else may run.
    // Not const though tidy finds it could be: it ends the entity, only
    // through the pointer generation() hands out.
    [[nodiscard]] bool kill(Entity entity) noexcept { // NOLINT(readability-make-member-function-const)
      if (!valid(entity)) {
        return false;
      }
      ++*generation(indexOf(entity));
      return true;
    }

    // The indices of entities kill ended, to the lists they came from in one
    // step each. Must have nothing running next to it.
    void giveMany(std::span<const uint32_t> indices) {
      openAll();
      LastList last;
      for (const uint32_t index: indices) {
        listOf(index, last).add(index);
      }
      closeAll();
    }

    // giveMany for the indices of many pages, each span those of one page,
    // on the calling thread and the pool: the lists get their final size
    // first, and every page is copied to its place by one thread. On one
    // thread giving 1M indices back took about 0.93 ms, a third of
    // World::parallelDestroy. The indices of a page all belong to its area,
    // so a page goes to one list in one piece.
    void giveMany(std::span<const std::span<const uint32_t>> pages, size_t helpers) {
      openAll();
      const uint32_t areas = m_areaCount.load(std::memory_order_relaxed);
      m_fills.resize(areas);
      for (uint32_t area = 0; area < areas; ++area) {
        const Area *const in = areaTable()[area].load(std::memory_order_relaxed);
        m_fills[area] = in != nullptr ? in->free.indices.size() : 0;
      }
      m_places.resize(pages.size());
      for (size_t i = 0; i < pages.size(); ++i) {
        if (pages[i].empty()) {
          continue;
        }
        size_t &at = m_fills[areaNumberOf(pages[i].front())];
        m_places[i] = at;
        at += pages[i].size();
      }
      for (uint32_t area = 0; area < areas; ++area) {
        if (Area *const in = areaTable()[area].load(std::memory_order_relaxed)) {
          in->free.indices.resize(m_fills[area]);
        }
      }
      threading::parallelFor(pages.size(), 1, helpers, [&](size_t first, size_t last) {
        for (size_t i = first; i < last; ++i) {
          if (!pages[i].empty()) {
            FreeList &list = areaOf(pages[i].front()).free;
            std::ranges::copy(pages[i], list.indices.begin() + static_cast<std::ptrdiff_t>(m_places[i]));
          }
        }
      });
      closeAll();
    }

    // How many pages were handed out to threads, and the number of each, in
    // the order they were handed out: a row that a loop can go through and
    // split up between threads. A page stays in it for good.
    [[nodiscard]] uint32_t pageCount() const noexcept { return m_pageCount.load(std::memory_order_acquire); }
    [[nodiscard]] uint32_t pageAt(uint32_t place) const noexcept { return order()[place]; }

    // The area of a page that was handed out.
    [[nodiscard]] uint32_t areaNumberOf(uint32_t index) const noexcept {
      return uint32_t{owners()[index >> PAGE_BITS].load(std::memory_order_relaxed)} - 1;
    }

    // The page area k starts at: k with its PAGE_NUMBER_BITS bits in reverse
    // order, past the last page that may be handed out at 0 again.
    [[nodiscard]] static constexpr uint32_t startOf(uint32_t area) noexcept {
      uint32_t page = 0;
      for (unsigned bit = 0; bit < PAGE_NUMBER_BITS; ++bit) {
        page |= ((area >> bit) & 1u) << (PAGE_NUMBER_BITS - 1 - bit);
      }
      return page < PAGE_COUNT ? page : 0;
    }

    // The generation of an index, followed by those of the indices after it up
    // to the end of their page. Null if no entity ever had an index near it.
    [[nodiscard]] uint32_t *generation(uint32_t index) const noexcept {
      uint32_t *block = blocks()[index >> BLOCK_BITS].load(std::memory_order_acquire);
      return block != nullptr ? block + (index & (BLOCK_SIZE - 1)) : nullptr;
    }

  private:
    [[nodiscard]] std::atomic<uint32_t *> *blocks() const noexcept {
      return reinterpret_cast<std::atomic<uint32_t *> *>(m_reserved);
    }
    [[nodiscard]] std::atomic<uint16_t> *owners() const noexcept {
      return reinterpret_cast<std::atomic<uint16_t> *>(m_reserved + OWNERS_AT);
    }
    [[nodiscard]] uint32_t *order() const noexcept { return reinterpret_cast<uint32_t *>(m_reserved + ORDER_AT); }
    [[nodiscard]] std::atomic<Area *> *areaTable() const noexcept {
      return reinterpret_cast<std::atomic<Area *> *>(m_reserved + AREAS_AT);
    }

    // The area with the number, made if it is not there yet.
    [[nodiscard]] Area &area(uint32_t number) {
      if (number >= AREA_LIMIT) [[unlikely]] {
        throw std::out_of_range("sc::ecs: area beyond EntityAllocator::AREA_LIMIT");
      }
      if (Area *const found = areaTable()[number].load(std::memory_order_acquire)) [[likely]] {
        return *found;
      }
      return makeArea(number);
    }

    // The area of an index that was handed out.
    [[nodiscard]] Area &areaOf(uint32_t index) const noexcept {
      return *areaTable()[areaNumberOf(index)].load(std::memory_order_relaxed);
    }

    // The free list of the page an index lies in, remembered for the next
    // index of the same page: the indices of a page mostly come one after the
    // other. Looking up the area of every index (its owner, then the table of
    // areas, then the list), World::destroy(span) of 100 000 entities with two
    // components took 1 020 instead of 785 µs.
    struct LastList {
      uint32_t page = ~uint32_t{0};
      FreeList *list = nullptr;
    };
    [[nodiscard]] FreeList &listOf(uint32_t index, LastList &last) const noexcept {
      if (index >> PAGE_BITS != last.page) [[unlikely]] {
        last.page = index >> PAGE_BITS;
        last.list = &areaOf(index).free;
      }
      return *last.list;
    }

    // The rest of area, for its first entity. Out of line, it is done once.
    [[gnu::noinline, gnu::cold]] Area &makeArea(uint32_t number) {
      const std::scoped_lock lock{m_pageMutex};
      std::atomic<Area *> &slot = areaTable()[number];
      if (Area *const found = slot.load(std::memory_order_relaxed)) {
        return *found;
      }
      auto *const made = new Area{startOf(number) >> m_startShift};
      slot.store(made, std::memory_order_release);
      if (m_areaCount.load(std::memory_order_relaxed) <= number) {
        m_areaCount.store(number + 1, std::memory_order_release);
      }
      return *made;
    }

    // open and close of the lists of all areas, see FreeList.
    void openAll() {
      const uint32_t areas = m_areaCount.load(std::memory_order_relaxed);
      for (uint32_t area = 0; area < areas; ++area) {
        if (Area *const in = areaTable()[area].load(std::memory_order_relaxed)) {
          in->free.open();
        }
      }
    }
    void closeAll() {
      const uint32_t areas = m_areaCount.load(std::memory_order_relaxed);
      for (uint32_t area = 0; area < areas; ++area) {
        if (Area *const in = areaTable()[area].load(std::memory_order_relaxed)) {
          in->free.close();
        }
      }
    }

    // Calls the function with the Share of the calling thread in the area.
    // The threads without one of their own, those outside the pool
    // (threading::NO_THREAD_ID) and pool threads beyond MAX_THREADS, use one
    // together, one at a time.
    template<typename Function>
    std::invoke_result_t<Function &, Share &> withShare(Area &area, Function &&function) {
      const uint32_t thread = threading::threadId();
      if (thread < MAX_THREADS) [[likely]] {
        return function(area.shares[thread]);
      }
      const std::scoped_lock lock{m_sharedMutex};
      return function(area.shares[MAX_THREADS]);
    }

    // The first index of a page no thread has indices of yet, for the area:
    // the first free one from its cursor on, past the last page at 0 again.
    // Throws std::length_error if there is none.
    // Under a lock: a page is handed out once per PAGE_SIZE entities, and the
    // row of pages must not get a gap that a loop would find.
    uint32_t makePage(Area &area, uint32_t number) {
      const std::scoped_lock lock{m_pageMutex};
      uint32_t page = area.cursor;
      for (uint32_t tried = 0; tried < m_pageLimit; ++tried) {
        if (owners()[page].load(std::memory_order_relaxed) == 0) {
          const uint32_t first = page << PAGE_BITS;
          makeBlock(first);
          area.cursor = page + 1 < m_pageLimit ? page + 1 : 0;
          const uint32_t place = m_pageCount.load(std::memory_order_relaxed);
          order()[place] = page;
          // After the generations: valid finds the page handed out only once
          // they are there.
          owners()[page].store(static_cast<uint16_t>(number + 1), std::memory_order_release);
          m_pageCount.store(place + 1, std::memory_order_release);
          return first;
        }
        page = page + 1 < m_pageLimit ? page + 1 : 0;
      }
      throw std::length_error("sc::ecs: no entity index is left");
    }

    // Makes sure the generations of the index and its page have memory.
    void makeBlock(uint32_t index) {
      std::atomic<uint32_t *> &slot = blocks()[index >> BLOCK_BITS];
      if (slot.load(std::memory_order_acquire) != nullptr) [[likely]] {
        return;
      }
      // Only under m_pageMutex, so no other thread makes it meanwhile.
      slot.store(static_cast<uint32_t *>(mi_zalloc_aligned(BLOCK_SIZE * sizeof(uint32_t), 64)),
                 std::memory_order_release);
    }

    // The tables, at the offsets above: the generations of every index
    // block, null until an index of it is handed out; the area of every page
    // plus one, 0 for one not handed out; the pages in the order they were
    // handed out; and every area, null until it is used.
    // How many pages are handed out at most, and how far the start of an
    // area is shifted down for that, see the constructor.
    uint32_t m_pageLimit;
    unsigned m_startShift;
    std::byte *m_reserved;
    Area m_common{startOf(COMMON_AREA)};
    alignas(64) std::atomic<uint32_t> m_pageCount{0};
    std::atomic<uint32_t> m_areaCount{0};
    // Held while a page or an area is made.
    std::mutex m_pageMutex;
    // For the threads that have no Share of their own, see withShare.
    std::mutex m_sharedMutex;
    // Where the indices of every page go in giveMany of pages, and how far
    // the list of every area is filled meanwhile.
    std::vector<size_t> m_places;
    std::vector<size_t> m_fills;
  };

} // namespace sc::ecs::internal
