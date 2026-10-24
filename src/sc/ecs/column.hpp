#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "sc/memory/range_cache.hpp"
#include "sc/memory/virtual_memory.hpp"

#include "entity.hpp"

namespace sc::ecs {

  // Says that few entities have a component:
  //
  //   template <> inline constexpr bool sc::ecs::rareComponent<Player> = true;
  //
  // Its column then gets memory in small pieces, a page of the column at a
  // time, instead of the huge pages that make going through many components
  // fast: a huge page would be held by a handful of components.
  template<typename Component>
  inline constexpr bool rareComponent = false;

  // What the OS backs the columns of a world with: World world{Pages::Small}.
  enum class Pages : uint8_t {
    // Chosen by each column, see the two below:
    //  - huge pages from the start for the components of at least
    //    HUGE_PAGE_COMPONENT_BYTES. A huge page holds few large components, so
    //    little of it is left unused, while their column would need a great
    //    many small pages, each of them fetched from the OS on its own;
    //  - for smaller components small pages until the column takes up
    //    HUGE_PAGE_COLUMN_BYTES, huge ones from then on, also for what it has
    //    already. A world with few entities gets the memory of small pages,
    //    one with many the speed of huge ones.
    Auto,
    // Huge pages where the OS has them. Fastest to go through, but a column
    // takes its memory in pieces of memory::HUGE_PAGE_SIZE, so a world with
    // few entities holds much more than its components need.
    Huge,
    // Only pages of memory::OS_PAGE_SIZE, for every component and not just
    // the rare ones. The memory follows the components closely.
    Small,
  };

  // What emplace, tryEmplace and emplaceOrReplace did. None of them hands
  // out a reference to the component: another thread may erase it as soon
  // as it is made. get it afterwards if it is needed.
  enum class Emplaced : uint8_t {
    // The component was made.
    Made,
    // There was one, and it got a new value (emplaceOrReplace).
    Replaced,
    // There was one, and the function changed it (emplaceOrModify).
    Modified,
    // There was one, or another thread was first, and it was left as it is
    // (tryEmplace).
    There,
  };

  // From which size on a component gets huge pages from the start in a world
  // of Pages::Auto. At this size a huge page holds the components of 8 pages
  // of a column.
  inline constexpr size_t HUGE_PAGE_COMPONENT_BYTES = 64;

  // How much memory the pages of a column of smaller components take before
  // it moves to huge pages in a world of Pages::Auto: as much as one of them
  // holds. A column that is filled without gaps then never holds more than a
  // huge page it does not need, and little of it has to be moved over.
  inline constexpr size_t HUGE_PAGE_COLUMN_BYTES = memory::HUGE_PAGE_SIZE;

  // How much memory of emptied pages a column keeps at most, for pages that
  // get components again, see Column::SPARE_PAGES.
  inline constexpr size_t SPARE_BYTES = 2 * memory::HUGE_PAGE_SIZE;

} // namespace sc::ecs

namespace sc::ecs::internal {

  // One bit for every entity of a page.
  inline constexpr size_t PAGE_WORDS = PAGE_SIZE / 64;
  static_assert(PAGE_WORDS <= 64, "the summary of a page has one bit for every word of its mask");
  // How many pages the indices are cut into.
  inline constexpr size_t PAGE_COUNT = INDEX_LIMIT >> PAGE_BITS;

  // How many indices a chunk of a column of components of the given size
  // has, as a power of two: enough for memory::HUGE_PAGE_SIZE, so that huge
  // pages fit into it, and at least 16 pages, so that a column has at most
  // 65 536 chunks.
  template<size_t ComponentBytes>
  constexpr unsigned chunkBits() noexcept {
    unsigned bits = PAGE_BITS + 4;
    while ((size_t{1} << bits) < INDEX_LIMIT && ComponentBytes * (size_t{1} << bits) < memory::HUGE_PAGE_SIZE) {
      ++bits;
    }
    return bits;
  }

  // The entities of one page that a destroy of many takes away: a bit in
  // words for every one of them, and a bit in touched for every word that
  // has one.
  struct KillPage {
    uint32_t number = 0;
    uint64_t touched = 0;
    std::array<uint64_t, PAGE_WORDS> words{};
  };

  struct ColumnBase;

#ifndef NDEBUG
  // What a loop of World on this thread goes through, and the entity whose
  // call it is in, for the assert in assertNotLooped. Only in debug builds.
  struct LoopWatch {
    // Where the world keeps the columns the loop names, excluded ones among
    // them: a column made only during the loop is found there as well.
    std::span<const std::atomic<ColumnBase *> *const> columns;
    uint32_t index;
    // Of the loop the function of which runs this one, or null.
    const LoopWatch *outer;
  };

  // The innermost loop on this thread, or null.
  inline thread_local const LoopWatch *loopWatch = nullptr;
#endif

  // Asserts that no loop on this thread names the column, unless it is in
  // the call for the index: the function of a loop does not touch the
  // components the loop names on other entities, excluded ones included. A
  // loop reads the mask of a word before it goes through it, and that of a
  // full page not at all, so one the function takes from an entity after its
  // own would be called destroyed; in parallelEach another thread may be
  // writing the one modify or emplaceOrReplace changes; and whether the loop
  // gets to an entity that one was given to or taken from would depend on
  // where it lies. Nothing in release builds.
  inline void assertNotLooped([[maybe_unused]] const ColumnBase *column, [[maybe_unused]] uint32_t index) noexcept {
#ifndef NDEBUG
    for (const LoopWatch *watch = loopWatch; watch != nullptr; watch = watch->outer) {
      for (const std::atomic<ColumnBase *> *const looped: watch->columns) {
        assert((looped->load(std::memory_order_relaxed) != column || index == watch->index) &&
               "sc::ecs: the function of a loop added, removed or modified a component the loop names on another "
               "entity; record that with Commands");
      }
    }
#endif
  }

  struct ColumnBase;

  // Components CommandApplier took from their entities, moved out of their
  // columns for their onRemove hooks, which run once all changes of the
  // stage are done. The values lie in blocks of its own.
  class RemovedStash {
  public:
    struct Removed {
      Entity entity;
      ColumnBase *column;
      // Null for a component without members.
      void *value;
    };

    // At most two rounds: a new block has room for any padding.
    [[nodiscard]] void *allocate(size_t bytes, size_t alignment) {
      for (;;) {
        if (!m_blocks.empty()) {
          void *at = m_blocks.back().get() + m_used;
          size_t space = m_blockSize - m_used;
          if (std::align(alignment, bytes, at, space) != nullptr) {
            m_used = m_blockSize - space + bytes;
            return at;
          }
        }
        m_blockSize = std::max(BLOCK_BYTES, bytes + alignment);
        m_blocks.push_back(std::make_unique_for_overwrite<std::byte[]>(m_blockSize));
        m_used = 0;
      }
    }

    void add(Entity entity, ColumnBase &column, void *value) {
      m_items.push_back({.entity = entity, .column = &column, .value = value});
    }

    [[nodiscard]] std::span<const Removed> items() const noexcept { return m_items; }

    // Once every value is gone, see ColumnBase::runStashed.
    void clear() noexcept {
      m_items.clear();
      m_blocks.clear();
      m_used = 0;
      m_blockSize = 0;
    }

  private:
    static constexpr size_t BLOCK_BYTES = size_t{64} << 10;

    std::vector<Removed> m_items;
    std::vector<std::unique_ptr<std::byte[]>> m_blocks;
    // Of the last block.
    size_t m_used = 0;
    size_t m_blockSize = 0;
  };

  // What a column knows about a page, apart from its values; the same for
  // every component, so that ColumnBase can read it. On cache lines of its
  // own: threads that keep to different pages do not slow each
  // other down.
  struct alignas(64) PageHeader {
    // Which entities of the page have the component.
    std::array<std::atomic<uint64_t>, PAGE_WORDS> mask;
    // Which of them a thread is making the component for with tryEmplace:
    // it is that thread's to make, and not in the mask before it is made.
    // Also set while erase destroys a component, so that the next one is
    // not made in its place before it is gone, while emplaceOrReplace
    // makes or assigns one, and while modify changes one.
    std::array<std::atomic<uint64_t>, PAGE_WORDS> making;
    // Which words of the mask may have a bit set: a loop only looks at
    // these. Set with the first bit of a word, but only cleared when the
    // page is empty, so a word may be empty all the same; clearing it with
    // every word that becomes empty made destroy up to 11 % slower.
    std::atomic<uint64_t> summary;
    std::atomic<uint32_t> count;
    // Whether the values have memory. Only changed by the thread that has
    // busy set.
    std::atomic<bool> started;
    // Set while a thread gives the values their memory or takes it back.
    std::atomic_flag busy;
    // Whether the page is one of the spares, see park.
    std::atomic<bool> parked;
    // Whether the values gave their memory back since the column has
    // them. Only used by the thread that has busy set.
    bool vacated;
  };

  // What the world needs of a column without knowing its component.
  struct ColumnBase {
    virtual ~ColumnBase() = default;
    // False if the entity had no component, at the index; the onRemove hooks
    // otherwise.
    virtual bool erase(Entity entity) noexcept = 0;
    // The same while no other thread uses the column.
    virtual bool eraseAlone(uint32_t index) noexcept = 0;
    // eraseAlone for the entities of the pages, a word at a time.
    virtual void eraseAlone(std::span<const KillPage> pages) noexcept = 0;
    // Counts so many components more in a page, and gives the page its
    // memory if it has none: for the adds a thread is about to do with
    // emplaceCounted, while no other thread changes the page in this column.
    virtual void occupyPage(uint32_t number, uint32_t count) = 0;
    // Counts so many less, after occupyPage for adds that were not done.
    virtual void forsakePage(uint32_t number, uint32_t count) noexcept = 0;
    // Calls the onAdd hooks with the component of the entity, if it has one,
    // under its making bit: for the adds CommandApplier did without hooks.
    virtual void runAdded(Entity entity) = 0;
    // The same for the onRemove hooks, of a component about to go with its
    // entity, see World::destroy.
    virtual void runRemoved(Entity entity) noexcept = 0;
    // Moves the component of the entity into the stash, if it has one, for
    // runStashed: CommandApplier, which takes it away right after.
    virtual void stash(Entity entity, RemovedStash &stash) = 0;
    // The onRemove hooks with a stashed component, which goes then.
    virtual void runStashed(Entity entity, void *value) noexcept = 0;
    // Only the stashed component goes, after an exception.
    virtual void dropStashed(void *value) noexcept = 0;

    // Whether the column has onRemove hooks. Set before the column is used.
    bool removeHooked = false;
  };

  // Where a column keeps its headers, so that World::destroy can ask whether
  // a column has an entity without a virtual call: with 100 component types,
  // 82 of the 100 virtual calls of eraseAlone per destroy went past the
  // prediction of the CPU (Ryzen 7 5800X), 900 ns per destroy against 45 ns
  // with 20 types. Kept by the world next to its list of columns, not in
  // ColumnBase: two pointers there moved m_headerBlocks of every column onto
  // a cache line of its own, away from m_hooks, which adding reads as well.
  struct ColumnPages {
    // The headers come in blocks of this many pages, as Column::HEADER_BLOCK_PAGES.
    static constexpr uint32_t BLOCK_PAGES = 64;

    const std::atomic<uint64_t> *blocks;
    const PageHeader *headers;

    // Whether the entity at the index has the component.
    [[nodiscard]] bool hasAt(uint32_t index) const noexcept {
      const PageHeader *const header = find(index >> PAGE_BITS);
      const uint32_t slot = index & (PAGE_SIZE - 1);
      return header != nullptr && ((header->mask[slot / 64].load(std::memory_order_relaxed) >> (slot % 64)) & 1u) != 0;
    }

    // Whether any of the kill pages has a component in the column. Only the
    // count of a page: a look per page, not per word, also for a span of many.
    [[nodiscard]] bool mayHave(std::span<const KillPage> pages) const noexcept {
      return std::ranges::any_of(pages, [this](const KillPage &kill) {
        const PageHeader *const header = find(kill.number);
        return header != nullptr && header->count.load(std::memory_order_relaxed) != 0;
      });
    }

  private:
    [[nodiscard]] const PageHeader *find(uint32_t number) const noexcept {
      const uint32_t block = number / BLOCK_PAGES;
      const uint64_t bits = blocks[block / 64].load(std::memory_order_acquire);
      return ((bits >> (block % 64)) & 1u) != 0 ? headers + number : nullptr;
    }
  };

#ifndef NDEBUG
  // The making bits the thread holds while hooks run, for the assert in
  // Column::take: a hook that goes for its own component, or for that of a
  // hook it set off in turn, would wait for itself. All of them, not only the
  // last: a hook of relate destroying a child under the bit of its parent's
  // list waited for that bit in the child's own hook, unseen.
  struct HeldBit {
    const std::atomic<uint64_t> *word;
    uint64_t bit;
  };
  inline thread_local std::vector<HeldBit> heldBits;

  [[nodiscard]] inline bool holds(const std::atomic<uint64_t> &making, uint64_t bit) noexcept {
    return std::ranges::any_of(heldBits,
                               [&](const HeldBit &held) { return held.word == &making && (held.bit & bit) != 0; });
  }
#endif

  // The components of one type, each at the index of its entity.
  //
  // The component of an index is always at the same place. The column
  // reserves the addresses for them a chunk of CHUNK_SIZE indices at a time,
  // the first time a page of the chunk gets memory, and keeps them until it
  // goes; within a chunk one page of the column lies right after the other. A
  // page gets memory with the first component in it and gives it back when
  // the last one is gone, so a range of indices without the component costs
  // nothing, neither memory nor time to go through. Only the last
  // SPARE_PAGES pages that were emptied keep theirs for a while, see park.
  // The same goes for what the column knows about the entities of a page,
  // its header: the headers come in blocks of HEADER_BLOCK_PAGES pages, made
  // when the first of them is used.
  //
  // The chunks come from memory::rangeCache and go back to it, with the
  // memory they have: a column made after another one went gets its memory
  // without the OS clearing it first. Such a chunk may hold memory in pages
  // without components that no page of the column counts, at most a chunk
  // of it, until the column goes; the cache offered it to the OS, which
  // takes it when it needs it. The headers are reserved anew, they must be
  // zero.
  //
  // The pages of a chunk lie without gaps on purpose. Memory that an
  // allocator hands out page by page has unused bytes between the pages, and
  // with them parts of the CPU cache that the column never gets to use.
  //
  // Reading is safe from any number of threads. Adding and removing is safe
  // for threads that keep to different indices, also next to threads that ask
  // whether an index has the component: what a page says about its entities
  // is changed in single steps that no thread sees half done.
  //
  // Several threads may also go for the same index, with tryEmplace,
  // emplaceOrReplace and erase, in any mix: one of them does it and the
  // others are told that they did not. An index has the component from when it is made in full until
  // just before it is destroyed, for every thread that asks.
  template<typename T>
  class Column final : public ColumnBase {
    static_assert(std::is_nothrow_destructible_v<T>, "a component is destroyed where nothing may be thrown");
    static_assert(!std::is_const_v<T> && !std::is_reference_v<T>);
    static_assert(alignof(T) <= memory::OS_PAGE_SIZE);

    using Header = PageHeader;

    // Has busy of a header set for as long as it exists.
    class Busy {
    public:
      explicit Busy(Header &header) noexcept : m_header(header) {
        while (m_header.busy.test_and_set(std::memory_order_acquire)) {
          m_header.busy.wait(true, std::memory_order_relaxed);
        }
      }
      ~Busy() {
        m_header.busy.clear(std::memory_order_release);
        m_header.busy.notify_one();
      }
      Busy(const Busy &) = delete;
      Busy &operator=(const Busy &) = delete;

    private:
      Header &m_header;
    };

  public:
    // A component without members has no instances, only the bits.
    static constexpr bool HAS_VALUE = !std::is_empty_v<T>;
    // Whether a component has something to do at its end, like giving back
    // the memory of a std::string. One that has not is just left behind.
    static constexpr bool DESTROYS = HAS_VALUE && !std::is_trivially_destructible_v<T>;
    // A page that has components, or nothing.
    struct Page {
      // Which entities of the page have the component. Null for no page.
      const std::atomic<uint64_t> *mask = nullptr;
      // Which words of the mask may have a bit set, see Header::summary.
      uint64_t summary = 0;
      uint32_t count = 0;
      // Null for a component without members.
      T *values = nullptr;

      explicit operator bool() const noexcept { return mask != nullptr; }
    };

    // A rare component gets small pages whatever the world asks for. Has no
    // memory of its own until a component is added, only the addresses of
    // its headers. Only made with new, see operator new.
    explicit Column(Pages pages = Pages::Huge) :
        m_hugePages(!rareComponent<T> &&
                    (pages == Pages::Huge || (pages == Pages::Auto && sizeof(T) >= HUGE_PAGE_COMPONENT_BYTES))),
        m_grows(!rareComponent<T> && pages == Pages::Auto && sizeof(T) < HUGE_PAGE_COMPONENT_BYTES) {
      // Windows commits nothing for no bytes, it fails.
      if constexpr (CHUNK_COUNT != 0) {
        memory::commit(m_chunkBytes, sizeof(m_chunkBytes));
      }
      for (std::atomic<uint32_t> &spare: m_spares) {
        spare.store(NO_PAGE, std::memory_order_relaxed);
      }
    }

    ~Column() override {
      if constexpr (DESTROYS) {
        // The pages that have memory, the spares among them, which have no
        // components. Only in the blocks of headers that have memory: one
        // page after the other took 1M steps for a rare entity.
        size_t left = pages();
        for (size_t word = 0; left != 0 && word < m_headerBlocks.size(); ++word) {
          for (uint64_t blocks = m_headerBlocks[word].load(std::memory_order_relaxed); left != 0 && blocks != 0;
               blocks &= blocks - 1) {
            const size_t block = (word * 64) + static_cast<size_t>(std::countr_zero(blocks));
            const auto first = static_cast<uint32_t>(block * HEADER_BLOCK_PAGES);
            const auto end = static_cast<uint32_t>(std::min(first + size_t{HEADER_BLOCK_PAGES}, PAGE_COUNT));
            for (uint32_t number = first; left != 0 && number < end; ++number) {
              const Header &header = headers()[number];
              if (!header.started.load(std::memory_order_relaxed)) {
                continue;
              }
              --left;
              T *const values = valuesOf(number);
              for (uint32_t at = 0; at < PAGE_WORDS; ++at) {
                for (uint64_t mask = header.mask[at].load(std::memory_order_relaxed); mask != 0; mask &= mask - 1) {
                  std::destroy_at(values + (at * 64) + static_cast<uint32_t>(std::countr_zero(mask)));
                }
              }
            }
          }
        }
      }
      // Up to the last one there is: a table of all chunks has no memory but
      // for those it got.
      for (size_t chunk = 0, left = chunks(); left != 0 && chunk < CHUNK_COUNT; ++chunk) {
        if (void *const reserved = chunkTable()[chunk].memory; reserved != nullptr) {
          memory::rangeCache().release(reserved, CHUNK_RESERVED_BYTES);
          --left;
        }
      }
      if (m_ticks != nullptr) {
        memory::release(m_ticks, TICK_BYTES);
      }
      // The headers and chunks have nothing to do at their end; their
      // memory goes with the column, see operator delete.
      static_assert(std::is_trivially_destructible_v<Header> && std::is_trivially_destructible_v<Chunk>);
    }

    // A column lies in addresses reserved for it alone, with the headers of
    // all pages and what it keeps per chunk in it, all zero from the OS and
    // without memory until used: about 1.06 GiB of them. Found at a fixed
    // place from the column, a component is one load away from it, as when
    // the chunks were a std::array of the column; behind a pointer to a range
    // of their own, a get of a random entity waited for one more load (the
    // lookup benchmarks of tests/ecs_compare, 21 to 27 % slower). On Linux
    // with vm.overcommit_memory=2 all of it is promised from the start, see
    // vacate. Not on the stack, which has no room for it.
    [[nodiscard]] static void *operator new(size_t bytes) {
      void *const reserved = memory::reserve(bytes);
      try {
        // The members before the headers and chunks, and maybe a little of
        // the chunks, which the constructor commits anyway.
        memory::commit(reserved, bytes - HEADER_BYTES - sizeof(m_chunkBytes));
      } catch (...) {
        memory::release(reserved, bytes);
        throw;
      }
      return reserved;
    }
    // A column is aligned to a cache line, a reserved range to a page of the
    // OS.
    [[nodiscard]] static void *operator new(size_t bytes, std::align_val_t) { return operator new(bytes); }
    static void operator delete(void *address, size_t bytes) noexcept { memory::release(address, bytes); }
    static void operator delete(void *address, size_t bytes, std::align_val_t) noexcept {
      memory::release(address, bytes);
    }

    Column(const Column &) = delete;
    Column &operator=(const Column &) = delete;

    // The page with the given number, nothing if no entity of it has the
    // component.
    [[nodiscard]] Page page(uint32_t number) const noexcept {
      assert(number < PAGE_COUNT);
      const Header *const header = findHeader(number);
      if (header == nullptr) {
        return {};
      }
      const uint32_t count = header->count.load(std::memory_order_relaxed);
      if (count == 0) {
        return {};
      }
      return {.mask = header->mask.data(),
              .summary = header->summary.load(std::memory_order_relaxed),
              .count = count,
              .values = valuesOf(number)};
    }

    [[nodiscard]] bool contains(uint32_t index) const noexcept {
      // Any index may be asked for, also that of an entity there never was.
      if (index >= INDEX_LIMIT) {
        return false;
      }
      const Header *const header = findHeader(index >> PAGE_BITS);
      return header != nullptr && has(*header, index & (PAGE_SIZE - 1));
    }

    // There must be a component at the index.
    [[nodiscard]] T &get(uint32_t index) const noexcept {
      assert(contains(index));
      return *valueAt(index);
    }

    // There must be no component at the index of the entity yet, and no
    // other thread that adds or removes the component of this index. Always
    // Emplaced::Made. If making the component throws, there is none at the
    // index afterwards, as before. Then the onAdd hooks, see added.
    //
    // The hooks are looked for first, and the rest is the same as without
    // them: with the look after the add, the entity was kept for it all the
    // way, and creating entities with two components on one thread took 988
    // instead of 934 µs (ecs_compare, 100K entities).
    template<typename... Args>
    Emplaced emplace(Entity entity, Args &&...args) {
      if (m_hooks != nullptr) [[unlikely]] {
        return emplaceHooked(entity, std::forward<Args>(args)...);
      }
      return emplaceAt(indexOf(entity), std::forward<Args>(args)...);
    }

  private:
    template<typename... Args>
    [[gnu::noinline]] Emplaced emplaceHooked(Entity entity, Args &&...args) {
      emplaceAt(indexOf(entity), std::forward<Args>(args)...);
      added(entity);
      return Emplaced::Made;
    }

    template<typename... Args>
    Emplaced emplaceAt(uint32_t index, [[maybe_unused]] Args &&...args) {
      assert(index < INDEX_LIMIT);
      const uint32_t number = index >> PAGE_BITS;
      Header &header = makeHeader(number);
      const uint32_t slot = index & (PAGE_SIZE - 1);
      assert(!has(header, slot));
      occupy(header, number, slot, 1);
      if constexpr (HAS_VALUE) {
        try {
          ::new (static_cast<void *>(valueAt(index))) T{std::forward<Args>(args)...};
        } catch (...) {
          forsake(header, number, 1);
          throw;
        }
      }
      // Only now: an index that has the component has one that was made.
      header.mask[slot / 64].fetch_or(uint64_t{1} << (slot % 64), std::memory_order_relaxed);
      markWord(header, slot / 64);
      touch(index);
      return Emplaced::Made;
    }

  public:
    // The same for an index that may have the component already, and that
    // other threads may be giving it or taking it from at the same time:
    // Emplaced::There if it had the component or another thread was first.
    // The component is that thread's then, with its arguments.
    // The onAdd hooks run before the bit is given back.
    template<typename... Args>
    Emplaced tryEmplace(Entity entity, [[maybe_unused]] Args &&...args) {
      const uint32_t index = indexOf(entity);
      assert(index < INDEX_LIMIT);
      const uint32_t number = index >> PAGE_BITS;
      Header &header = makeHeader(number);
      const uint32_t slot = index & (PAGE_SIZE - 1);
      if (has(header, slot)) {
        return Emplaced::There;
      }
      // The index is this thread's to make the component for, or another
      // thread is at it.
      std::atomic<uint64_t> &making = header.making[slot / 64];
      const uint64_t bit = uint64_t{1} << (slot % 64);
      if ((making.fetch_or(bit, std::memory_order_acq_rel) & bit) != 0) {
        return Emplaced::There;
      }
      // Asked again: a thread may have made it and be done since.
      if (has(header, slot)) {
        making.fetch_and(~bit, std::memory_order_release);
        return Emplaced::There;
      }
      try {
        occupy(header, number, slot, 1);
        if constexpr (HAS_VALUE) {
          try {
            ::new (static_cast<void *>(valueAt(index))) T{std::forward<Args>(args)...};
          } catch (...) {
            forsake(header, number, 1);
            throw;
          }
        }
      } catch (...) {
        making.fetch_and(~bit, std::memory_order_release);
        throw;
      }
      // There for everybody from here on, and so before the next thread may
      // try.
      header.mask[slot / 64].fetch_or(bit, std::memory_order_acq_rel);
      markWord(header, slot / 64);
      touch(index);
      if (m_hooks != nullptr) [[unlikely]] {
        try {
          runAddHooks(entity, &making, bit);
        } catch (...) {
          making.fetch_and(~bit, std::memory_order_release);
          throw;
        }
      }
      making.fetch_and(~bit, std::memory_order_release);
      return Emplaced::Made;
    }

    // The same for an index that keeps the component it has: it gets a new
    // one, made from the arguments and assigned to the one there, which stays
    // as it was if making it throws: Emplaced::Made or Emplaced::Replaced.
    // The making bit is held from the look at the mask until the component
    // is made or assigned, so another thread that goes for the index with
    // tryEmplace is told it was not first, and one with emplaceOrReplace
    // waits, as does erase of a component that DESTROYS. The page keeps its
    // memory under the assignment for the bit, see heldHas.
    template<typename... Args>
    Emplaced emplaceOrReplace(Entity entity, Args &&...args) {
      const bool made = emplaceOr(
          entity,
          [&](T *value) {
            assert(!m_replaceForbidden && "sc::ecs: this component is not replaced, see forbidReplace");
            if constexpr (HAS_VALUE) {
              *value = T{std::forward<Args>(args)...};
            }
          },
          std::forward<Args>(args)...);
      return made ? Emplaced::Made : Emplaced::Replaced;
    }

    // The same, but if there is a component, the function is called with it,
    // as modify does: Emplaced::Made or Emplaced::Modified. Of threads that
    // do this for the same index at the same time, one makes the component
    // and the others change it, each after the one before.
    template<typename Function, typename... Args>
    Emplaced emplaceOrModify(Entity entity, Function &&function, Args &&...args) {
      static_assert(HAS_VALUE, "a component without members has no value to modify");
      static_assert(std::is_invocable_v<Function &, T &>, "the function takes the component by reference");
      const bool made = emplaceOr(entity, [&](T *value) { function(*value); }, std::forward<Args>(args)...);
      return made ? Emplaced::Made : Emplaced::Modified;
    }

  private:
    // emplaceOrReplace and emplaceOrModify: true if the component was made,
    // false if there was one and there was called with it. The onAdd hooks
    // of one that was made run before the bit is given back.
    template<typename There, typename... Args>
    bool emplaceOr(Entity entity, const There &there, Args &&...args) {
      const uint32_t index = indexOf(entity);
      assert(index < INDEX_LIMIT);
      const uint32_t number = index >> PAGE_BITS;
      Header &header = makeHeader(number);
      const uint32_t slot = index & (PAGE_SIZE - 1);
      std::atomic<uint64_t> &making = header.making[slot / 64];
      const uint64_t bit = uint64_t{1} << (slot % 64);
      take(making, bit);
      bool made = false;
      try {
        if (heldHas(header, slot)) {
          // Changing it, unlike adding, is checked here.
          assertNotLooped(this, index);
          if constexpr (HAS_VALUE) {
            there(valueAt(index));
            touch(index);
          } else {
            // No values, and so no address for one, and nothing that changed:
            // a tag replaced is no change for Changed, as through Commands.
            there(nullptr);
          }
        } else {
          occupy(header, number, slot, 1);
          if constexpr (HAS_VALUE) {
            try {
              ::new (static_cast<void *>(valueAt(index))) T{std::forward<Args>(args)...};
            } catch (...) {
              forsake(header, number, 1);
              throw;
            }
          }
          header.mask[slot / 64].fetch_or(bit, std::memory_order_acq_rel);
          markWord(header, slot / 64);
          touch(index);
          made = true;
          if (m_hooks != nullptr) [[unlikely]] {
            runAddHooks(entity, &making, bit);
          }
        }
      } catch (...) {
        making.fetch_and(~bit, std::memory_order_release);
        throw;
      }
      making.fetch_and(~bit, std::memory_order_release);
      return made;
    }

  public:
    // Calls the function with the component at the index while it holds the
    // making bit. Another thread that modifies the same index waits, as do
    // emplaceOrReplace and erase of a component that DESTROYS, so of two
    // threads that change the same component, the second sees what the first
    // did. False, without a call, if the index has no component. The page
    // keeps its memory under the function for the bit, see heldHas.
    //
    // Loops and get do not take the bit, so nothing may read or write the
    // component at the index past modify while it may run.
    template<typename Function>
    bool modify(uint32_t index, Function &&function) {
      static_assert(HAS_VALUE, "a component without members has no value to modify");
      static_assert(std::is_invocable_v<Function &, T &>, "the function takes the component by reference");
      assert(index < INDEX_LIMIT);
      const uint32_t number = index >> PAGE_BITS;
      Header *const found = findHeader(number);
      const uint32_t slot = index & (PAGE_SIZE - 1);
      // Mostly the answer, and without a locked instruction.
      if (found == nullptr || !has(*found, slot)) {
        return false;
      }
      assertNotLooped(this, index);
      Header &header = *found;
      std::atomic<uint64_t> &making = header.making[slot / 64];
      const uint64_t bit = uint64_t{1} << (slot % 64);
      take(making, bit);
      bool called = false;
      try {
        // Asked again under the bit: an erase may have taken it meanwhile.
        if (heldHas(header, slot)) {
          function(*valueAt(index));
          touch(index);
          called = true;
        }
      } catch (...) {
        making.fetch_and(~bit, std::memory_order_release);
        throw;
      }
      making.fetch_and(~bit, std::memory_order_release);
      return called;
    }

    // Says that so many components are about to be added. In a world of
    // Pages::Auto the column moves to huge pages now if they would take
    // it there, so that their memory is not fetched in small pages first and
    // moved over afterwards.
    void expect(size_t count) {
      if constexpr (HAS_VALUE) {
        if (m_grows && !hugePages() && (pages() * PAGE_BYTES) + (count * sizeof(T)) >= HUGE_PAGE_COLUMN_BYTES) {
          moveToHugePages();
        }
      }
    }

    // A copy of the value at every one of count indices in a row, none of
    // which has the component yet. The same as emplace for each of them, but
    // with one step per page instead of one per component.
    void fill(uint32_t index, uint32_t count, [[maybe_unused]] const T &value) {
      assert(size_t{index} + count <= INDEX_LIMIT);
      while (count != 0) {
        const uint32_t here = std::min(count, PAGE_SIZE - (index & (PAGE_SIZE - 1)));
        [[maybe_unused]] T *const values = startRun(index, here);
        if constexpr (HAS_VALUE) {
          try {
            std::uninitialized_fill_n(values, here, value);
          } catch (...) {
            // The ones before stay, these are all gone again.
            abandonRun(index, here);
            throw;
          }
        }
        finishRun(index, here);
        index += here;
        count -= here;
      }
    }

    // fill in three steps, for values that are made one by one: startRun
    // counts count indices in a row from index on, all in one page and none
    // with the component yet, and gives the place of their values (null for
    // a component without members); once every value is made there,
    // finishRun gives them the component. If making one throws, the caller
    // destroys those it made and calls abandonRun instead.
    [[nodiscard]] T *startRun(uint32_t index, uint32_t count) {
      assert(count != 0 && (index & (PAGE_SIZE - 1)) + count <= PAGE_SIZE);
      const uint32_t number = index >> PAGE_BITS;
      occupy(makeHeader(number), number, index & (PAGE_SIZE - 1), count);
      if constexpr (HAS_VALUE) {
        return valueAt(index);
      } else {
        return nullptr;
      }
    }

    void finishRun(uint32_t index, uint32_t count) noexcept {
      Header &header = headers()[index >> PAGE_BITS];
      const uint32_t slot = index & (PAGE_SIZE - 1);
      for (uint32_t bit = slot; bit < slot + count;) {
        const uint32_t width = std::min(64 - (bit % 64), slot + count - bit);
        const uint64_t bits = (width == 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1) << (bit % 64);
        [[maybe_unused]] const uint64_t before = header.mask[bit / 64].fetch_or(bits, std::memory_order_relaxed);
        assert((before & bits) == 0);
        markWord(header, bit / 64);
        touch((index & ~(PAGE_SIZE - 1)) + bit);
        bit += width;
      }
    }

    void abandonRun(uint32_t index, uint32_t count) noexcept {
      const uint32_t number = index >> PAGE_BITS;
      forsake(headers()[number], number, count);
    }

    // m_hooks is what Edit looks at too, so that inlined into it, one look
    // decides both: with removeHooked here, adding and removing 100K
    // components on all threads through Edit took 127 instead of 108 µs.
    bool erase(Entity entity) noexcept override {
      if (m_hooks != nullptr) [[unlikely]] {
        return eraseHooked(entity);
      }
      return eraseAt(indexOf(entity));
    }

  private:
    // erase with onRemove hooks, which run while the making bit is held,
    // also for a component that does not DESTROYS: no tryEmplace of another
    // thread makes a new one where the hooks read the old one. A hook that
    // throws ends the program, as the component is gone from the mask by
    // then.
    [[gnu::noinline]] bool eraseHooked(Entity entity) noexcept {
      const uint32_t index = indexOf(entity);
      if (!removeHooked) {
        return eraseAt(index);
      }
      if (index >= INDEX_LIMIT) {
        return false;
      }
      const uint32_t number = index >> PAGE_BITS;
      Header *const found = findHeader(number);
      const uint32_t slot = index & (PAGE_SIZE - 1);
      if (found == nullptr || !has(*found, slot)) {
        return false;
      }
      assertNotLooped(this, index);
      Header &header = *found;
      std::atomic<uint64_t> &making = header.making[slot / 64];
      const uint64_t bit = uint64_t{1} << (slot % 64);
      take(making, bit);
      const bool removed = (header.mask[slot / 64].fetch_and(~bit, std::memory_order_acq_rel) & bit) != 0;
      if (removed) {
        callHooks(m_hooks->removed, entity, valueOf(index), &making, bit);
        if constexpr (DESTROYS) {
          std::destroy_at(valueAt(index));
        }
      }
      making.fetch_and(~bit, std::memory_order_release);
      if (removed) {
        forsake(header, number, 1);
      }
      return removed;
    }

    bool eraseAt(uint32_t index) noexcept {
      if (index >= INDEX_LIMIT) {
        return false;
      }
      const uint32_t number = index >> PAGE_BITS;
      Header *const found = findHeader(number);
      const uint32_t slot = index & (PAGE_SIZE - 1);
      if (found == nullptr || !has(*found, slot)) {
        return false;
      }
      assertNotLooped(this, index);
      Header &header = *found;
      const uint64_t bit = uint64_t{1} << (slot % 64);
      if constexpr (DESTROYS) {
        // Not for long: whoever has it is about to be done with the index.
        std::atomic<uint64_t> &making = header.making[slot / 64];
        while ((making.fetch_or(bit, std::memory_order_acq_rel) & bit) != 0) {
          std::this_thread::yield();
        }
        const bool removed = (header.mask[slot / 64].fetch_and(~bit, std::memory_order_acq_rel) & bit) != 0;
        if (removed) {
          // Before the page may give its memory back.
          std::destroy_at(valueAt(index));
        }
        making.fetch_and(~bit, std::memory_order_release);
        if (!removed) {
          // Another thread removed it just now.
          return false;
        }
      } else if ((header.mask[slot / 64].fetch_and(~bit, std::memory_order_seq_cst) & bit) == 0) {
        // seq_cst for heldHas: no bit taken here.
        return false;
      }
      forsake(header, number, 1);
      return true;
    }

  public:
    // An add that occupyPage counted already, by the only thread that changes
    // the page in this column, as CommandApplier gives a page to one thread:
    // no making bit, and the mask and the summary with a plain load and
    // store, as in eraseAlone (park and vacate on other threads leave the
    // summary alone). False, with nothing made, if the index has the
    // component.
    template<typename... Args>
    bool emplaceCounted(uint32_t index, [[maybe_unused]] Args &&...args) {
      assert(index < INDEX_LIMIT);
      Header *const header = findHeader(index >> PAGE_BITS);
      assert(header != nullptr && "sc::ecs: emplaceCounted without occupyPage");
      const uint32_t slot = index & (PAGE_SIZE - 1);
      if (has(*header, slot)) {
        return false;
      }
      if constexpr (HAS_VALUE) {
        ::new (static_cast<void *>(valueAt(index))) T{std::forward<Args>(args)...};
      }
      std::atomic<uint64_t> &word = header->mask[slot / 64];
      word.store(word.load(std::memory_order_relaxed) | (uint64_t{1} << (slot % 64)), std::memory_order_relaxed);
      const uint64_t summary = header->summary.load(std::memory_order_relaxed);
      header->summary.store(summary | (uint64_t{1} << (slot / 64)), std::memory_order_relaxed);
      touch(index);
      return true;
    }

    void occupyPage(uint32_t number, uint32_t count) override {
      assert(count != 0);
      occupy(makeHeader(number), number, 0, count);
    }

    void forsakePage(uint32_t number, uint32_t count) noexcept override {
      if (count != 0) {
        forsake(headers()[number], number, count);
      }
    }

    // Called with an entity and its component once the entity has one, or
    // while it still has it, see World::onAdd and World::onRemove: on
    // whichever thread adds or removes it, on several at once.
    using Hook = std::move_only_function<void(Entity, T &) const>;

    // Not while the column is used.
    void addHook(Hook hook) {
      static_assert(HAS_VALUE || std::is_default_constructible_v<T>,
                    "a hook of a component without members gets one made for the call");
      hooks().added.push_back(std::move(hook));
    }
    void addRemoveHook(Hook hook) {
      static_assert(HAS_VALUE || std::is_default_constructible_v<T>,
                    "a hook of a component without members gets one made for the call");
      static_assert(std::is_move_constructible_v<T>,
                    "a component removed through Commands is moved out for its onRemove hooks");
      hooks().removed.push_back(std::move(hook));
      removeHooked = true;
    }

    [[nodiscard]] bool hasAddHooks() const noexcept { return m_hooks != nullptr && !m_hooks->added.empty(); }
    [[nodiscard]] bool hasHooks() const noexcept { return m_hooks != nullptr; }

    // Keeps a tick of the clock for every word of 64 indices of every page,
    // from then on: the time the component of one of them was last added or
    // may have been changed, see World::trackChanges. Its own range of
    // addresses, given memory with the blocks of headers, so a column that
    // does not track costs nothing but a look at m_ticks. Not while the
    // column is used; components there before count as unchanged.
    void trackChanges(const std::atomic<uint32_t> &clock) {
      if (m_ticks != nullptr) {
        return;
      }
      auto *const ticks = static_cast<std::atomic<uint32_t> *>(memory::reserve(TICK_BYTES));
      for (size_t word = 0; word < m_headerBlocks.size(); ++word) {
        for (uint64_t blocks = m_headerBlocks[word].load(std::memory_order_relaxed); blocks != 0;
             blocks &= blocks - 1) {
          const size_t block = (word * 64) + static_cast<size_t>(std::countr_zero(blocks));
          memory::commit(ticks + (block * TICKS_PER_BLOCK), TICK_BLOCK_BYTES);
        }
      }
      m_clock = &clock;
      m_ticks = ticks;
    }

    [[nodiscard]] bool tracksChanges() const noexcept { return m_ticks != nullptr; }

    // The component at the index was added, or may have been changed.
    void touch(uint32_t index) const noexcept {
      if (m_ticks != nullptr) [[unlikely]] {
        m_ticks[index >> 6].store(m_clock->load(std::memory_order_relaxed), std::memory_order_relaxed);
      }
    }

    // The same for the words of a page, a bit for each, for a loop that may
    // have written them. The column must track changes.
    void touchWords(uint32_t number, uint64_t words) const noexcept {
      const uint32_t tick = m_clock->load(std::memory_order_relaxed);
      std::atomic<uint32_t> *const ticks = m_ticks + (size_t{number} * PAGE_WORDS);
      for (; words != 0; words &= words - 1) {
        ticks[std::countr_zero(words)].store(tick, std::memory_order_relaxed);
      }
    }

    // The words of a page with a tick after since, a bit for each: compared
    // across the overflow, so that a tick that went past 2^32 still counts
    // as later, as long as since is less than 2^31 ticks back. The column
    // must track changes and have the page.
    [[nodiscard]] uint64_t changedWords(uint32_t number, uint32_t since) const noexcept {
      const std::atomic<uint32_t> *const ticks = m_ticks + (size_t{number} * PAGE_WORDS);
      uint64_t words = 0;
      for (uint32_t word = 0; word < PAGE_WORDS; ++word) {
        const auto ahead = static_cast<int32_t>(ticks[word].load(std::memory_order_relaxed) - since);
        words |= uint64_t{ahead > 0} << word;
      }
      return words;
    }

    // For a component whose hooks keep something else in step with it, which
    // a replace would get past (no hook runs for it): emplaceOrReplace of
    // one that is there asserts in debug builds, also through Edit and
    // Commands. Not while the column is used.
    void forbidReplace() noexcept { m_replaceForbidden = true; }
    [[nodiscard]] bool replaceForbidden() const noexcept { return m_replaceForbidden; }

    // The onAdd hooks of a component the entity just got without the making
    // bit, from the only thread that may go for it.
    void added(Entity entity) {
      if (m_hooks != nullptr) [[unlikely]] {
        runAddHooks(entity, nullptr, 0);
      }
    }

    void runRemoved(Entity entity) noexcept override {
      if (!removeHooked) {
        return;
      }
      const uint32_t index = indexOf(entity);
      Header *const header = findHeader(index >> PAGE_BITS);
      if (header == nullptr) {
        return;
      }
      const uint32_t slot = index & (PAGE_SIZE - 1);
      std::atomic<uint64_t> &making = header->making[slot / 64];
      const uint64_t bit = uint64_t{1} << (slot % 64);
      take(making, bit);
      if (heldHas(*header, slot)) {
        callHooks(m_hooks->removed, entity, valueOf(index), &making, bit);
      }
      making.fetch_and(~bit, std::memory_order_release);
    }

    void stash(Entity entity, RemovedStash &stash) override {
      const uint32_t index = indexOf(entity);
      if (!contains(index)) {
        return;
      }
      if constexpr (HAS_VALUE && std::is_move_constructible_v<T>) {
        void *const at = stash.allocate(sizeof(T), alignof(T));
        ::new (at) T(std::move(*valueAt(index)));
        stash.add(entity, *this, at);
      } else {
        stash.add(entity, *this, nullptr);
      }
    }

    void runStashed(Entity entity, void *value) noexcept override {
      T *const stashed = static_cast<T *>(value);
      callHooks(m_hooks->removed, entity, stashed, nullptr, 0);
      dropStashed(value);
    }

    void dropStashed(void *value) noexcept override {
      if constexpr (HAS_VALUE) {
        std::destroy_at(static_cast<T *>(value));
      }
    }

    void runAdded(Entity entity) override {
      if (!hasAddHooks()) {
        return;
      }
      const uint32_t index = indexOf(entity);
      Header *const header = findHeader(index >> PAGE_BITS);
      if (header == nullptr) {
        return;
      }
      const uint32_t slot = index & (PAGE_SIZE - 1);
      std::atomic<uint64_t> &making = header->making[slot / 64];
      const uint64_t bit = uint64_t{1} << (slot % 64);
      take(making, bit);
      try {
        if (heldHas(*header, slot)) {
          runAddHooks(entity, &making, bit);
        }
      } catch (...) {
        making.fetch_and(~bit, std::memory_order_release);
        throw;
      }
      making.fetch_and(~bit, std::memory_order_release);
    }

    // The same without the making bit, also for a component that DESTROYS:
    // no other thread may go for the index at the same time, as for
    // LocalEdit. The mask and the count stay atomic, as other threads change
    // other indices of the page. A component that owns a unique_ptr, 1M
    // entities one after the other, ns per erase: 7.7 with the bit, 4.2
    // without.
    // The onRemove hooks run before the component goes, without the bit.
    bool eraseWithoutBit(Entity entity) noexcept {
      const uint32_t index = indexOf(entity);
      if (index >= INDEX_LIMIT) {
        return false;
      }
      const uint32_t number = index >> PAGE_BITS;
      Header *const found = findHeader(number);
      const uint32_t slot = index & (PAGE_SIZE - 1);
      if (found == nullptr || !has(*found, slot)) {
        return false;
      }
      assertNotLooped(this, index);
      Header &header = *found;
      const uint64_t bit = uint64_t{1} << (slot % 64);
      // seq_cst for heldHas, as erase without the bit.
      if ((header.mask[slot / 64].fetch_and(~bit, std::memory_order_seq_cst) & bit) == 0) {
        return false;
      }
      if (removeHooked) [[unlikely]] {
        callHooks(m_hooks->removed, entity, valueOf(index), nullptr, 0);
      }
      if constexpr (DESTROYS) {
        // Before the page may give its memory back.
        std::destroy_at(valueAt(index));
      }
      forsake(header, number, 1);
      return true;
    }

    // The same as erase while no other thread uses the column, for
    // World::destroy: no making bit to wait for, and the mask and the count
    // each with a relaxed load and store, which on x86 are plain
    // instructions, instead of a locked one: a destroy of an entity with two
    // components took 10 % less (BM_DestroyEntities of ecs_benchmark, gcc,
    // 9.4 instead of 10.4 ns at 262144 entities).
    bool eraseAlone(uint32_t index) noexcept override {
      if (index >= INDEX_LIMIT) {
        return false;
      }
      const uint32_t number = index >> PAGE_BITS;
      Header *const found = findHeader(number);
      const uint32_t slot = index & (PAGE_SIZE - 1);
      if (found == nullptr || !has(*found, slot)) {
        return false;
      }
      assertNotLooped(this, index);
      Header &header = *found;
      std::atomic<uint64_t> &word = header.mask[slot / 64];
      word.store(word.load(std::memory_order_relaxed) & ~(uint64_t{1} << (slot % 64)), std::memory_order_relaxed);
      if constexpr (DESTROYS) {
        // Before the page may give its memory back.
        std::destroy_at(valueAt(index));
      }
      const uint32_t left = header.count.load(std::memory_order_relaxed) - 1;
      header.count.store(left, std::memory_order_relaxed);
      if (left == 0) [[unlikely]] {
        // Nobody adds a component meanwhile: no word has a bit any more.
        header.summary.store(0, std::memory_order_relaxed);
        if constexpr (HAS_VALUE) {
          park(header, number);
        } else {
          vacate(header, number);
        }
      }
      return true;
    }

    // Per page one header, per word of it one change of the mask for up to 64
    // entities, and the count once: what eraseAlone does per entity. Only the
    // words that may have a bit in the column are looked at, and a word that
    // is empty afterwards leaves the summary, which is exact here.
    //
    // Only no other thread may use these pages; other threads may change
    // other pages of the column at the same time, as when CommandApplier
    // gives every page to one thread. So the count goes down with forsake,
    // one locked instruction per page: a page emptied elsewhere may leave the
    // spares and look at the count of this one in vacate.
    void eraseAlone(std::span<const KillPage> pages) noexcept override {
      for (const KillPage &kill: pages) {
        Header *const header = findHeader(kill.number);
        if (header == nullptr) {
          continue;
        }
        uint64_t summary = header->summary.load(std::memory_order_relaxed);
        uint32_t removed = 0;
        for (uint64_t touched = kill.touched & summary; touched != 0; touched &= touched - 1) {
          const auto word = static_cast<uint32_t>(std::countr_zero(touched));
          const uint64_t mask = header->mask[word].load(std::memory_order_relaxed);
          const uint64_t gone = mask & kill.words[word];
          if (gone == 0) {
            continue;
          }
#ifndef NDEBUG
          for (uint64_t bits = gone; bits != 0; bits &= bits - 1) {
            assertNotLooped(this,
                            (kill.number << PAGE_BITS) + (word * 64) + static_cast<uint32_t>(std::countr_zero(bits)));
          }
#endif
          header->mask[word].store(mask & ~gone, std::memory_order_relaxed);
          if constexpr (DESTROYS) {
            const uint32_t first = (kill.number << PAGE_BITS) + (word * 64);
            for (uint64_t bits = gone; bits != 0; bits &= bits - 1) {
              std::destroy_at(valueAt(first + static_cast<uint32_t>(std::countr_zero(bits))));
            }
          }
          if (gone == mask) {
            summary &= ~(uint64_t{1} << word);
          }
          removed += static_cast<uint32_t>(std::popcount(gone));
        }
        if (removed == 0) {
          continue;
        }
        header->summary.store(summary, std::memory_order_relaxed);
        forsake(*header, kill.number, removed);
      }
    }

    // Whether the OS may hand the values out in huge pages.
    [[nodiscard]] bool hugePages() const noexcept { return m_hugePages.load(std::memory_order_relaxed); }

    // How many pages have memory, the spares among them.
    [[nodiscard]] size_t pages() const noexcept { return m_pages.load(std::memory_order_relaxed); }

    // How many pages have memory but no component, kept for when they get
    // one again, see park. Exact only while no thread adds or removes.
    [[nodiscard]] size_t spares() const noexcept {
      size_t found = 0;
      for (const std::atomic<uint32_t> &spare: m_spares) {
        const uint32_t number = spare.load(std::memory_order_relaxed);
        found += number != NO_PAGE && headers()[number].count.load(std::memory_order_relaxed) == 0 &&
                 headers()[number].started.load(std::memory_order_relaxed);
      }
      return found;
    }

    // Gives the memory of the spares back now. Safe next to threads that add
    // and remove: a spare that got a component meanwhile keeps its memory.
    void trim() noexcept {
      for (std::atomic<uint32_t> &spare: m_spares) {
        const uint32_t number = spare.exchange(NO_PAGE, std::memory_order_acq_rel);
        if (number != NO_PAGE) {
          unpark(number);
        }
      }
    }

    // How many blocks of headers have memory, see HEADER_BLOCK_PAGES. They
    // keep it until the column goes.
    [[nodiscard]] size_t headerBlocks() const noexcept {
      size_t count = 0;
      for (const std::atomic<uint64_t> &word: m_headerBlocks) {
        count += static_cast<size_t>(std::popcount(word.load(std::memory_order_relaxed)));
      }
      return count;
    }

    // Where the headers are, see ColumnPages. They stay where they are for as
    // long as the column exists.
    [[nodiscard]] ColumnPages headerPages() const noexcept {
      return {.blocks = m_headerBlocks.data(), .headers = headers()};
    }

    // How many chunks of the values have their addresses reserved, see
    // CHUNK_SIZE. They stay until the column goes.
    [[nodiscard]] size_t chunks() const noexcept { return m_chunkCount.load(std::memory_order_relaxed); }

    // The headers come in blocks of this many pages.
    static constexpr uint32_t HEADER_BLOCK_PAGES = ColumnPages::BLOCK_PAGES;
    // The values are reserved in chunks of this many indices.
    static constexpr uint32_t CHUNK_SIZE = uint32_t{1} << chunkBits<sizeof(T)>();
    // How many emptied pages keep their memory at most: 4, fewer where that
    // would be more than SPARE_BYTES, but at least one. A component without
    // members has no memory to keep.
    static constexpr size_t SPARE_PAGES =
        HAS_VALUE ? std::clamp<size_t>(SPARE_BYTES / (sizeof(T) * PAGE_SIZE), 1, 4) : 0;

  private:
    // The values of a page are a whole number of memory::OS_PAGE_SIZE, so a
    // page gets and gives back its memory without touching its neighbours.
    // Where the pages of the OS are larger and a small component makes the
    // two share one, the shared ones stay with the column: memory::decommit
    // only gives back what belongs to the page alone.
    static constexpr size_t PAGE_BYTES = sizeof(T) * PAGE_SIZE;
    static_assert(!HAS_VALUE || PAGE_BYTES % memory::OS_PAGE_SIZE == 0);

    // The last block is short of a page: INDEX_LIMIT leaves the last page
    // out.
    static constexpr size_t HEADER_BLOCK_COUNT = (PAGE_COUNT + HEADER_BLOCK_PAGES - 1) / HEADER_BLOCK_PAGES;
    static constexpr size_t HEADER_BLOCK_BYTES = sizeof(Header) * HEADER_BLOCK_PAGES;
    static constexpr size_t HEADER_BYTES = HEADER_BLOCK_BYTES * HEADER_BLOCK_COUNT;
    // The ticks of trackChanges, a block of them with every block of headers.
    static constexpr size_t TICKS_PER_BLOCK = size_t{HEADER_BLOCK_PAGES} * PAGE_WORDS;
    static constexpr size_t TICK_BLOCK_BYTES = TICKS_PER_BLOCK * sizeof(uint32_t);
    static constexpr size_t TICK_BYTES = TICK_BLOCK_BYTES * HEADER_BLOCK_COUNT;
    // A block gets its memory without touching its neighbours.
    static_assert(HEADER_BLOCK_BYTES % memory::OS_PAGE_SIZE == 0);

    static constexpr unsigned CHUNK_BITS = chunkBits<sizeof(T)>();
    static constexpr uint32_t CHUNK_PAGES = CHUNK_SIZE >> PAGE_BITS;
    // The last one is short of the last page, as the last block of headers.
    static constexpr size_t CHUNK_COUNT = HAS_VALUE ? (size_t{INDEX_LIMIT} + CHUNK_SIZE - 1) >> CHUNK_BITS : 0;
    static constexpr size_t CHUNK_BYTES = PAGE_BYTES * CHUNK_PAGES;
    // With room to move the values to where a huge page may start.
    static constexpr size_t CHUNK_RESERVED_BYTES = CHUNK_BYTES + memory::HUGE_PAGE_SIZE;

    // The OS may hand the values out in huge pages, each of which holds
    // several pages of the column or a part of one. A chunk starts at a huge
    // page; its last one may be cut short where the chunk is not a whole
    // number of them.
    static constexpr size_t CHUNK_REGIONS = (CHUNK_BYTES + memory::HUGE_PAGE_SIZE - 1) / memory::HUGE_PAGE_SIZE;
    // What Chunk::regions says about a huge page.
    static constexpr uint32_t UNTOUCHED = 0;
    static constexpr uint32_t STARTING = 1;
    static constexpr uint32_t STARTED = 2;
    // An empty place among the spares.
    static constexpr uint32_t NO_PAGE = ~uint32_t{0};

    // What the column knows about a chunk, all zero until it is reserved.
    struct Chunk {
      // The values, at a huge page, null until the chunk is reserved.
      std::atomic<T *> values;
      // The range reserved for it, which they lie in.
      void *memory;
      // Whether memory::rangeCache kept that range, see startPage.
      bool kept;
      // Whether the first access to each of its huge pages was made.
      std::array<std::atomic<uint32_t>, CHUNK_REGIONS> regions;
    };
    // At least a byte: a column of a component without members has no
    // chunks.
    static constexpr size_t CHUNK_TABLE_BYTES = std::max<size_t>(CHUNK_COUNT * sizeof(Chunk), 1);

    [[nodiscard]] static bool has(const Header &header, uint32_t slot) noexcept {
      return ((header.mask[slot / 64].load(std::memory_order_relaxed) >> (slot % 64)) & 1u) != 0;
    }

    // has for an index whose making bit this thread took with take, in
    // emplaceOrReplace and modify, which then use the value without counting
    // it in the page. Counting it, with occupy and forsake, made modify 2.6
    // times slower where threads hit different entities of the same page:
    // all of them wrote the count of that page (one of 1 000 entities, all
    // threads, 37.8 instead of 14.5 ns per modify, ecs_compare). What keeps
    // the page from giving its memory back under them is the bit instead:
    // vacate looks at the making bits after it found the count at zero, and
    // keeps the memory if one is set.
    //
    // erase of a component without DESTROYS takes no bit, so it may take the
    // component meanwhile. Then either vacate sees the bit, or this sees the
    // component gone and leaves the value alone. Here the bit is set before
    // the mask is read; there the mask is cleared before the count, which
    // vacate reads before the bits. All of them are seq_cst, so they are in
    // one order that agrees with each thread's, and the two reads cannot both
    // come before the other side's write. On x86 the same instructions as
    // with weaker orders.
    [[nodiscard]] static bool heldHas(const Header &header, uint32_t slot) noexcept {
      return ((header.mask[slot / 64].load(std::memory_order_seq_cst) >> (slot % 64)) & 1u) != 0;
    }

    // Whether a thread holds a making bit of the page, for vacate: one in
    // emplaceOrReplace or modify may be using a value it does not count, see
    // heldHas. After the count, and seq_cst for that. 64 loads, on the rare
    // way of a page that gives its memory back.
    [[nodiscard]] static bool anyHeld(const Header &header) noexcept {
      return std::ranges::any_of(
          header.making, [](const std::atomic<uint64_t> &word) { return word.load(std::memory_order_seq_cst) != 0; });
    }

    // Takes the making bit of an index, waiting while another thread has it,
    // which is about to be done with the index. seq_cst for heldHas.
    static void take(std::atomic<uint64_t> &making, uint64_t bit) noexcept {
      assert(!holds(making, bit) &&
             "sc::ecs: a hook went for the component of the entity it was called for, or one whose hook set it off; "
             "change it through the reference it got");
      while ((making.fetch_or(bit, std::memory_order_seq_cst) & bit) != 0) {
        std::this_thread::yield();
      }
    }

    // Notes in the summary that a word of the mask has a bit now. Only
    // after the bit: a loop that finds the word in the summary also finds
    // the bit. Most components go into a word that has others already, and
    // for these the load is all.
    static void markWord(Header &header, uint32_t word) noexcept {
      const uint64_t bit = uint64_t{1} << word;
      if ((header.summary.load(std::memory_order_relaxed) & bit) == 0) {
        header.summary.fetch_or(bit, std::memory_order_acq_rel);
      }
    }

    // The header of every page and what the column keeps of every chunk, in
    // the column itself, see operator new.
    [[nodiscard]] Header *headers() const noexcept { return reinterpret_cast<Header *>(m_headerBytes); }
    [[nodiscard]] Chunk *chunkTable() const noexcept { return reinterpret_cast<Chunk *>(m_chunkBytes); }

    // The header of a page, null as long as no page of its block was used.
    // Always inlined: since the hooks call it from more places, GCC kept it
    // out of line, also in CommandApplier, whose add + remove of 100K
    // components ran 3 % more instructions for it.
    [[nodiscard, gnu::always_inline]] Header *findHeader(uint32_t number) const noexcept {
      const uint32_t block = number / HEADER_BLOCK_PAGES;
      const uint64_t blocks = m_headerBlocks[block / 64].load(std::memory_order_acquire);
      return ((blocks >> (block % 64)) & 1u) != 0 ? headers() + number : nullptr;
    }

    // The same, with the block given its memory if it has none yet. Throws
    // std::bad_alloc if there is no memory for it.
    [[nodiscard]] Header &makeHeader(uint32_t number) {
      const uint32_t block = number / HEADER_BLOCK_PAGES;
      const uint64_t bit = uint64_t{1} << (block % 64);
      if ((m_headerBlocks[block / 64].load(std::memory_order_acquire) & bit) == 0) [[unlikely]] {
        startHeaderBlock(block);
      }
      return headers()[number];
    }

    // Gives a block of headers its memory, all zero, which makes all of its
    // headers empty. Without a lock: several threads may do it for the same
    // block at the same time, memory::commit allows that, and on Linux it is
    // nothing at all; the pages come with the first access, in whichever
    // thread makes it. A lock here had the threads of a parallel loop that
    // all start with the same block sleep and wake up again: 36 % more
    // context switches and a 15 % slower parallel create (tests/ecs_compare).
    // Out of line, so that what adds a component stays small enough to be
    // inlined where it is called.
    [[gnu::noinline]] void startHeaderBlock(size_t block) {
      memory::commit(headers() + (block * HEADER_BLOCK_PAGES), HEADER_BLOCK_BYTES);
      if (m_ticks != nullptr) {
        memory::commit(m_ticks + (block * TICKS_PER_BLOCK), TICK_BLOCK_BYTES);
      }
      m_headerBlocks[block / 64].fetch_or(uint64_t{1} << (block % 64), std::memory_order_acq_rel);
    }

    // Where the component of an index is, made or not. Its chunk must be
    // reserved. Relaxed: a thread that may touch the component has seen the
    // page get its memory, and with it the chunk.
    [[nodiscard]] T *valueAt(uint32_t index) const noexcept {
      return chunkTable()[index >> CHUNK_BITS].values.load(std::memory_order_relaxed) + (index & (CHUNK_SIZE - 1));
    }

    // The values of a page, null for a component without members and for a
    // page whose chunk was never reserved.
    [[nodiscard]] T *valuesOf(uint32_t number) const noexcept {
      if constexpr (HAS_VALUE) {
        T *const values = chunkTable()[number / CHUNK_PAGES].values.load(std::memory_order_relaxed);
        return values != nullptr ? values + (size_t{number % CHUNK_PAGES} << PAGE_BITS) : nullptr;
      } else {
        return nullptr;
      }
    }

    // The values of a chunk, reserved if they are not yet. Throws
    // std::bad_alloc if the OS has no room for them. Out of line, as
    // makeHeaderBlock.
    [[gnu::noinline]] T *makeChunk(size_t chunk) {
      std::atomic<T *> &slot = chunkTable()[chunk].values;
      T *values = slot.load(std::memory_order_acquire);
      if (values != nullptr) [[likely]] {
        return values;
      }
      // One thread reserves it, the others wait: a reservation that is given
      // back again would hold up every thread of the process that has a page
      // fault just then, the OS keeps its mappings under one lock.
      const std::scoped_lock lock{m_growMutex};
      values = slot.load(std::memory_order_relaxed);
      if (values != nullptr) {
        return values;
      }
      bool kept = false;
      void *const reserved = memory::rangeCache().reserve(CHUNK_RESERVED_BYTES, kept);
      // Where a huge page may start: an address, not an offset, the range
      // itself may begin anywhere.
      const auto address = reinterpret_cast<uintptr_t>(reserved);
      const uintptr_t aligned = (address + memory::HUGE_PAGE_SIZE - 1) & ~(memory::HUGE_PAGE_SIZE - 1);
      auto *const fresh = reinterpret_cast<T *>(static_cast<std::byte *>(reserved) + (aligned - address));
      // A chunk reserved while the column moves to huge pages may keep small
      // ones, as a page started just then does, see moveToHugePages.
      if (hugePages()) {
        memory::preferHugePages(fresh, CHUNK_BYTES);
      } else {
        memory::preferSmallPages(fresh, CHUNK_BYTES);
      }
      // Before the chunk is there for others, who read it after they saw it.
      chunkTable()[chunk].kept = kept;
      slot.store(fresh, std::memory_order_release);
      // Only read when the column goes, after every thread is done with it.
      chunkTable()[chunk].memory = reserved;
      m_chunkCount.fetch_add(1, std::memory_order_relaxed);
      return fresh;
    }

    // Counts so many components more in a page, from slot on, which is all
    // there is to it unless the page has no memory: the values get it then. A
    // spare has it, also for its first component.
    //
    // Other threads may add to the page and remove from it at the same time.
    // A thread that finds the memory missing waits here for the one that
    // fetches it, or that is taking it back, see vacate. One that finds it
    // there may use it: vacate marks it as gone before it looks at the count,
    // which it finds raised then.
    void occupy(Header &header, uint32_t number, uint32_t slot, uint32_t count) {
#ifndef NDEBUG
      for (uint32_t i = 0; i < count; ++i) {
        assertNotLooped(this, (number << PAGE_BITS) + slot + i);
      }
#endif
      header.count.fetch_add(count, std::memory_order_seq_cst);
      if (header.started.load(std::memory_order_seq_cst)) [[likely]] {
        return;
      }
      start(header, number, slot, count);
    }

    // The rest of occupy, for the first components of a page. Out of line, as
    // makeHeaderBlock.
    [[gnu::noinline]] void start(Header &header, uint32_t number, uint32_t slot, uint32_t count) {
      const Busy busy{header};
      if (header.started.load(std::memory_order_relaxed)) {
        return;
      }
      if constexpr (HAS_VALUE) {
        try {
          startPage(number, slot, count, header.vacated);
        } catch (...) {
          header.count.fetch_sub(count, std::memory_order_seq_cst);
          throw;
        }
      }
      m_pages.fetch_add(1, std::memory_order_relaxed);
      header.started.store(true, std::memory_order_seq_cst);
    }

    // Counts so many components less in a page, which are gone already.
    // The rest out of line, so that this stays small enough to be inlined
    // where a component is removed: with the erase of onRemove hooks next to
    // it, GCC kept it out of the loop of Edit::erase, which ran 11 % more
    // instructions for it (100K components added and removed on all
    // threads).
    void forsake(Header &header, uint32_t number, uint32_t count) noexcept {
      if (header.count.fetch_sub(count, std::memory_order_seq_cst) == count) [[unlikely]] {
        emptied(header, number);
      }
    }

    // A page whose count reached zero in forsake.
    [[gnu::noinline, gnu::cold]] void emptied(Header &header, uint32_t number) noexcept {
      // No word has a bit any more. A thread that gave the page a component
      // meanwhile may have found its word still in the summary and left it as
      // it was: the summary is made anew from the mask then.
      header.summary.store(0, std::memory_order_seq_cst);
      if (header.count.load(std::memory_order_seq_cst) != 0) {
        for (uint32_t word = 0; word < PAGE_WORDS; ++word) {
          if (header.mask[word].load(std::memory_order_seq_cst) != 0) {
            header.summary.fetch_or(uint64_t{1} << word, std::memory_order_seq_cst);
          }
        }
      }
      if constexpr (HAS_VALUE) {
        park(header, number);
      } else {
        vacate(header, number);
      }
    }

    // A page that was just emptied keeps its memory as one of the spares,
    // and the one it takes the place of gives its own back, unless it got
    // components again meanwhile. Returning the memory of a page at once
    // and fetching it again with its next component took 2.3 µs with huge
    // pages and 22 µs with small ones per component added and removed again
    // (16 byte components, a page with just one entity); as a spare it
    // takes 9 to 12 ns, 7 ns in a page that holds other components as well.
    // With huge pages it also cut the huge page the page lies in into small
    // ones for good. The spares keep that from a few pages whose components
    // come and go all the time, more than one so that two of them do not
    // take each other's place over and over.
    //
    // A page is among the spares once at most: parked is set before it goes
    // in and cleared after it came out. A page emptied again while it is
    // still in has nothing to do; one emptied while it comes out goes in
    // again or finds its memory gone in unpark.
    //
    // parked is sequentially consistent, as the count: a thread that finds
    // it set came before unpark clears it, and so did its count, which
    // vacate sees then. Only looked at first, which on x86 costs nothing,
    // while the exchange would add a locked instruction to every page that
    // is emptied again while it is in: 11 to 14 instead of 9 to 12 ns per
    // component added and removed again.
    void park(Header &header, uint32_t number) noexcept {
      if (header.parked.load(std::memory_order_seq_cst) || header.parked.exchange(true, std::memory_order_seq_cst)) {
        return;
      }
      const uint32_t place = m_nextSpare.fetch_add(1, std::memory_order_relaxed) % SPARE_PAGES;
      const uint32_t replaced = m_spares[place].exchange(number, std::memory_order_acq_rel);
      if (replaced != NO_PAGE) {
        unpark(replaced);
      }
    }

    // A page that came out of the spares gives its memory back if it still
    // has no component, see park.
    void unpark(uint32_t number) noexcept {
      Header &header = headers()[number];
      header.parked.store(false, std::memory_order_seq_cst);
      vacate(header, number);
    }

    // A page has no component: its memory goes back, unless a thread has
    // added one since, or holds a making bit of it (see heldHas).
    //
    // Whoever adds one counts it first and looks for the memory after, and
    // here the memory is marked as gone first and the count looked at after.
    // So either the count seen here is not zero and the memory stays, or the
    // other thread finds the memory gone and waits in occupy until it is.
    //
    // A page that keeps its memory for a held bit has no component and is not
    // among the spares any more: it keeps it until it gets a component and
    // is emptied again. One page at most each time an erase empties a page
    // while modify or emplaceOrReplace holds a bit of it, and the page leaves
    // the spares before that thread is done.
    //
    // On Windows the promise of memory goes back with it, see
    // memory::decommit. On Linux with vm.overcommit_memory=2 that promise is
    // made for the whole chunk when it is reserved and kept until the column
    // goes: taking it back page by page would take mprotect, which cuts the
    // mapping of the chunk into pieces, and the kernel allows a process only
    // vm.max_map_count of them.
    void vacate(Header &header, uint32_t number) noexcept {
      const Busy busy{header};
      if (!header.started.exchange(false, std::memory_order_seq_cst)) {
        // Another thread that removed a last component was here already.
        return;
      }
      if (header.count.load(std::memory_order_seq_cst) != 0 || anyHeld(header)) {
        header.started.store(true, std::memory_order_seq_cst);
        return;
      }
      if constexpr (HAS_VALUE) {
        memory::decommit(valuesOf(number), PAGE_BYTES);
        header.vacated = true;
      }
      m_pages.fetch_sub(1, std::memory_order_relaxed);
    }

    // Gives the values of a page their memory.
    //
    // The memory of a huge page comes with the first access to it, all of it
    // at once and cleared. Threads that start pages of the column in the same
    // huge page would each fetch and clear one, all but one of them for
    // nothing. So the first thread makes that access alone and the others
    // wait for it, which they would have to do in the OS anyway. From then on
    // nobody waits for anybody: a page that gets memory again after it gave
    // its own back only fetches that.
    //
    // With small pages the memory of the page is its own. What the page
    // starts with, the count components from slot on, is fetched in one go
    // when it spans more than one OS_PAGE_SIZE: left to the first accesses,
    // the OS would be asked once for every OS_PAGE_SIZE of it, 1.5 instead of
    // 2.5 ms to create 100 000 entities with 320 bytes of components each in
    // small pages. The rest of the page comes with its first accesses: all of
    // it in one go held as much memory for one component in every page as for
    // a full column, 63 MB instead of 1.3 MB for 245 components of 64 bytes
    // spread over 1M entities. Not in a chunk that memory::rangeCache kept,
    // nor in a page that gave its memory back before (memory::decommit): the
    // memory is there already, or offered to the OS and still there unless it
    // took it. Writing to it is all it takes then, while fetching it in one
    // go has the OS go through every piece of it: refilling 100 000 entities
    // with two components took 218 instead of 157 µs. On Windows decommit
    // takes the memory at once, but there populate does nothing anyway.
    void startPage(uint32_t number, uint32_t slot, uint32_t count, bool vacated) {
      const size_t chunk = number / CHUNK_PAGES;
      auto *const base = reinterpret_cast<std::byte *>(makeChunk(chunk));
      // Within the chunk.
      const size_t offset = size_t{number % CHUNK_PAGES} * PAGE_BYTES;
      std::byte *const first = base + offset;
      memory::commit(first, PAGE_BYTES);
      // Within the chunk, as offset.
      const size_t firstRegion = offset / memory::HUGE_PAGE_SIZE;
      const size_t lastRegion = (offset + PAGE_BYTES - 1) / memory::HUGE_PAGE_SIZE;
      std::array<std::atomic<uint32_t>, CHUNK_REGIONS> &regions = chunkTable()[chunk].regions;

      if (!hugePages()) {
        if (m_grows && (pages() + 1) * PAGE_BYTES >= HUGE_PAGE_COLUMN_BYTES) {
          moveToHugePages();
        } else {
          // Remembered for moveToHugePages. Whoever comes to the huge page
          // later finds it started.
          for (size_t region = firstRegion; region <= lastRegion; ++region) {
            regions[region].store(STARTED, std::memory_order_release);
          }
          const size_t bytes = size_t{count} * sizeof(T);
          if (!vacated && !chunkTable()[chunk].kept && bytes > memory::OS_PAGE_SIZE) {
            memory::populate(first + (size_t{slot} * sizeof(T)), bytes);
          }
          return;
        }
      }

      for (size_t region = firstRegion; region <= lastRegion; ++region) {
        std::atomic<uint32_t> &state = regions[region];
        uint32_t seen = state.load(std::memory_order_acquire);
        if (seen == STARTED) [[likely]] {
          continue;
        }
        if (seen == UNTOUCHED &&
            state.compare_exchange_strong(seen, STARTING, std::memory_order_acq_rel, std::memory_order_acquire)) {
          // Within this page: the rest of the huge page belongs to others.
          const size_t touched = std::max(offset, region * memory::HUGE_PAGE_SIZE);
          *reinterpret_cast<volatile std::byte *>(base + touched) = std::byte{0};
          state.store(STARTED, std::memory_order_release);
          state.notify_all();
          continue;
        }
        while (seen != STARTED) {
          state.wait(seen, std::memory_order_acquire);
          seen = state.load(std::memory_order_acquire);
        }
      }
    }

    // Makes the column one of huge pages, for good: what it has in small
    // pages is moved over, and new memory comes in huge ones. Threads that
    // start other pages meanwhile are not in the way; a page started with
    // small pages just then keeps them.
    void moveToHugePages() {
      if (m_hugePages.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      // Up to the last one there is, as when the column goes. Under the lock
      // that chunks are made under, so that none comes in between and the
      // count is right; one made afterwards is made for huge pages anyway.
      const std::scoped_lock lock{m_growMutex};
      for (size_t chunk = 0, left = chunks(); left != 0 && chunk < CHUNK_COUNT; ++chunk) {
        T *const values = chunkTable()[chunk].values.load(std::memory_order_acquire);
        if (values == nullptr) {
          continue;
        }
        --left;
        memory::preferHugePages(values, CHUNK_BYTES);
        auto *const base = reinterpret_cast<std::byte *>(values);
        // Only the huge pages that lie in the chunk as a whole.
        for (size_t region = 0; (region + 1) * memory::HUGE_PAGE_SIZE <= CHUNK_BYTES; ++region) {
          if (chunkTable()[chunk].regions[region].load(std::memory_order_acquire) == STARTED) {
            memory::collapse(base + (region * memory::HUGE_PAGE_SIZE), memory::HUGE_PAGE_SIZE);
          }
        }
      }
    }

    struct Hooks {
      std::vector<Hook> added;
      std::vector<Hook> removed;
    };

    [[nodiscard]] Hooks &hooks() {
      if (m_hooks == nullptr) {
        m_hooks = std::make_unique<Hooks>();
      }
      return *m_hooks;
    }

    // The value at the index, null for a component without members.
    [[nodiscard]] T *valueOf([[maybe_unused]] uint32_t index) const noexcept {
      if constexpr (HAS_VALUE) {
        return valueAt(index);
      } else {
        return nullptr;
      }
    }

    // The onAdd hooks with the component of the entity, under the making
    // bit if making is not null.
    [[gnu::noinline, gnu::cold]] void runAddHooks(Entity entity, const std::atomic<uint64_t> *making, uint64_t bit) {
      callHooks(m_hooks->added, entity, valueOf(indexOf(entity)), making, bit);
    }

    // The hooks with the value, one made for the call for a component
    // without members.
    void callHooks(const std::vector<Hook> &hooks, Entity entity, T *value,
                   [[maybe_unused]] const std::atomic<uint64_t> *making, [[maybe_unused]] uint64_t bit) {
#ifndef NDEBUG
      struct Hold {
        explicit Hold(const std::atomic<uint64_t> *making, uint64_t bit) {
          if (making != nullptr) {
            heldBits.push_back({.word = making, .bit = bit});
          }
          held = making != nullptr;
        }
        ~Hold() {
          if (held) {
            heldBits.pop_back();
          }
        }
        bool held;
      } hold{making, bit};
#endif
      if constexpr (HAS_VALUE) {
        for (const Hook &hook: hooks) {
          hook(entity, *value);
        }
      } else if constexpr (std::is_default_constructible_v<T>) {
        T none{};
        for (const Hook &hook: hooks) {
          hook(entity, none);
        }
      }
    }

    // Changes once at most, from small pages to huge ones, see Pages::Auto.
    std::atomic<bool> m_hugePages{true};
    bool m_grows = false;
    // In the gap before m_hooks, which leaves m_headerBlocks where it was.
    bool m_replaceForbidden = false;
    // Null as long as nobody gave the column a hook: that is all that adding
    // a component looks at then, next to m_headerBlocks, which it reads
    // anyway.
    std::unique_ptr<Hooks> m_hooks;
    // See trackChanges: null as long as the column does not track them.
    std::atomic<uint32_t> *m_ticks = nullptr;
    const std::atomic<uint32_t> *m_clock = nullptr;
    // A bit for every block of headers that has memory, see m_headerBytes.
    std::array<std::atomic<uint64_t>, (HEADER_BLOCK_COUNT + 63) / 64> m_headerBlocks{};
    std::atomic<size_t> m_chunkCount{0};
    // Held while a chunk is reserved.
    std::mutex m_growMutex;
    std::atomic<size_t> m_pages{0};
    // The emptied pages that keep their memory, NO_PAGE for an empty place;
    // the next one takes the place m_nextSpare points to.
    std::array<std::atomic<uint32_t>, SPARE_PAGES> m_spares;
    std::atomic<uint32_t> m_nextSpare{0};
    // Last, and left as the OS gives them, all zero; see operator new. What
    // the column keeps of every chunk, see Chunk. Filled with zeros by the
    // constructor, it was 1.6 MiB for a component of 64 bytes in every
    // column, whether used or not. In ranges of their own, one per table,
    // making and dropping a world of two columns took 106 instead of 45 µs,
    // nearly all of it in mmap and munmap.
    alignas(Chunk) mutable std::byte m_chunkBytes[CHUNK_TABLE_BYTES];
    // The header of every page, given memory a block of HEADER_BLOCK_PAGES at
    // a time, when a page of the block is first used.
    alignas(Header) mutable std::byte m_headerBytes[HEADER_BYTES];
  };

} // namespace sc::ecs::internal
