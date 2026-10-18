#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>

#include "sc/memory/range_cache.hpp"
#include "sc/memory/virtual_memory.hpp"

#include "entity.hpp"

namespace sc::ecs2 {

  // Says that few entities have a component:
  //
  //   template <> inline constexpr bool sc::ecs2::rareComponent<Player> = true;
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

} // namespace sc::ecs2

namespace sc::ecs2::internal {

  // One bit for every entity of a page.
  inline constexpr size_t PAGE_WORDS = PAGE_SIZE / 64;
  static_assert(PAGE_WORDS <= 64, "the summary of a page has one bit for every word of its mask");
  // How many pages the indices are cut into.
  inline constexpr size_t PAGE_COUNT = INDEX_LIMIT >> PAGE_BITS;

  // How many indices a chunk of a column of components of the given size
  // has, as a power of two: enough for memory::HUGE_PAGE_SIZE, so that huge
  // pages fit into it, and at least 16 pages, so that a column has at most
  // 256 chunks.
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

  // What the world needs of a column without knowing its component.
  struct ColumnBase {
    virtual ~ColumnBase() = default;
    // False if there was no component at the index.
    virtual bool erase(uint32_t index) noexcept = 0;
    // The same while no other thread uses the column.
    virtual bool eraseAlone(uint32_t index) noexcept = 0;
    // eraseAlone for the entities of the pages, a word at a time.
    virtual void eraseAlone(std::span<const KillPage> pages) noexcept = 0;
  };

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

    // What the column knows about a page, apart from its values. On cache
    // lines of its own: threads that keep to different pages do not slow each
    // other down.
    struct alignas(64) Header {
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
    // its headers.
    explicit Column(Pages pages = Pages::Huge) :
        m_hugePages(!rareComponent<T> &&
                    (pages == Pages::Huge || (pages == Pages::Auto && sizeof(T) >= HUGE_PAGE_COMPONENT_BYTES))),
        m_grows(!rareComponent<T> && pages == Pages::Auto && sizeof(T) < HUGE_PAGE_COMPONENT_BYTES),
        m_headers(static_cast<Header *>(memory::reserve(HEADER_BYTES))) {
      for (std::atomic<uint32_t> &spare: m_spares) {
        spare.store(NO_PAGE, std::memory_order_relaxed);
      }
    }

    ~Column() override {
      if constexpr (DESTROYS) {
        // The pages that have memory, the spares among them, which have no
        // components.
        size_t left = pages();
        for (uint32_t number = 0; left != 0 && number < PAGE_COUNT; ++number) {
          const Header *const header = findHeader(number);
          if (header == nullptr || !header->started.load(std::memory_order_relaxed)) {
            continue;
          }
          --left;
          T *const values = valuesOf(number);
          for (uint32_t word = 0; word < PAGE_WORDS; ++word) {
            for (uint64_t mask = header->mask[word].load(std::memory_order_relaxed); mask != 0; mask &= mask - 1) {
              std::destroy_at(values + (word * 64) + static_cast<uint32_t>(std::countr_zero(mask)));
            }
          }
        }
      }
      for (void *const reserved: m_chunkMemory) {
        if (reserved != nullptr) {
          memory::rangeCache().release(reserved, CHUNK_RESERVED_BYTES);
        }
      }
      // The headers have nothing to do at their end.
      static_assert(std::is_trivially_destructible_v<Header>);
      memory::release(m_headers, HEADER_BYTES);
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

    // There must be no component at the index yet, and no other thread that
    // adds or removes the component of this index. Always Emplaced::Made. If
    // making the component throws, there is none at the index afterwards, as
    // before.
    template<typename... Args>
    Emplaced emplace(uint32_t index, [[maybe_unused]] Args &&...args) {
      assert(index < INDEX_LIMIT);
      const uint32_t number = index >> PAGE_BITS;
      Header &header = makeHeader(number);
      const uint32_t slot = index & (PAGE_SIZE - 1);
      assert(!has(header, slot));
      occupy(header, number, 1);
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
      return Emplaced::Made;
    }

    // The same for an index that may have the component already, and that
    // other threads may be giving it or taking it from at the same time:
    // Emplaced::There if it had the component or another thread was first.
    // The component is that thread's then, with its arguments.
    template<typename... Args>
    Emplaced tryEmplace(uint32_t index, [[maybe_unused]] Args &&...args) {
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
        occupy(header, number, 1);
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
      making.fetch_and(~bit, std::memory_order_release);
      return Emplaced::Made;
    }

    // The same for an index that keeps the component it has: it gets a new
    // one, made from the arguments and assigned to the one there, which stays
    // as it was if making it throws: Emplaced::Made or Emplaced::Replaced.
    // The making bit is held from the look at the mask until the component
    // is made or assigned, so another thread that goes for the index with
    // tryEmplace is told it was not first, and one with emplaceOrReplace
    // waits, as does erase of a component that DESTROYS.
    template<typename... Args>
    Emplaced emplaceOrReplace(uint32_t index, Args &&...args) {
      assert(index < INDEX_LIMIT);
      const uint32_t number = index >> PAGE_BITS;
      Header &header = makeHeader(number);
      const uint32_t slot = index & (PAGE_SIZE - 1);
      std::atomic<uint64_t> &making = header.making[slot / 64];
      const uint64_t bit = uint64_t{1} << (slot % 64);
      // Not for long, as in erase.
      while ((making.fetch_or(bit, std::memory_order_acq_rel) & bit) != 0) {
        std::this_thread::yield();
      }
      bool made = false;
      try {
        // Counted before the look at the mask, also for a component that is
        // there: erase of one that does not DESTROYS takes no making bit, and
        // if it took the last of the page, the page could give its memory
        // back under the assignment. Counted, it keeps it.
        occupy(header, number, 1);
        try {
          if (has(header, slot)) {
            if constexpr (HAS_VALUE) {
              *valueAt(index) = T{std::forward<Args>(args)...};
            }
          } else {
            if constexpr (HAS_VALUE) {
              ::new (static_cast<void *>(valueAt(index))) T{std::forward<Args>(args)...};
            }
            header.mask[slot / 64].fetch_or(bit, std::memory_order_acq_rel);
            markWord(header, slot / 64);
            made = true;
          }
        } catch (...) {
          forsake(header, number, 1);
          throw;
        }
        if (!made) {
          // The one there is counted already.
          forsake(header, number, 1);
        }
      } catch (...) {
        making.fetch_and(~bit, std::memory_order_release);
        throw;
      }
      making.fetch_and(~bit, std::memory_order_release);
      return made ? Emplaced::Made : Emplaced::Replaced;
    }

    // Calls the function with the component at the index while it holds the
    // making bit. Another thread that modifies the same index waits, as do
    // emplaceOrReplace and erase of a component that DESTROYS, so of two
    // threads that change the same component, the second sees what the first
    // did. False, without a call, if the index has no component. Counted in
    // the page while the function runs, as in emplaceOrReplace: erase of a
    // component without DESTROYS takes no bit, and if it took the last one of
    // the page, the page could give its memory back under the function.
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
      Header &header = *found;
      std::atomic<uint64_t> &making = header.making[slot / 64];
      const uint64_t bit = uint64_t{1} << (slot % 64);
      // Not for long, as in erase.
      while ((making.fetch_or(bit, std::memory_order_acq_rel) & bit) != 0) {
        std::this_thread::yield();
      }
      bool called = false;
      try {
        occupy(header, number, 1);
        try {
          // Asked again under the bit: an erase may have taken it meanwhile.
          if (has(header, slot)) {
            function(*valueAt(index));
            called = true;
          }
        } catch (...) {
          forsake(header, number, 1);
          throw;
        }
        forsake(header, number, 1);
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
        const uint32_t number = index >> PAGE_BITS;
        Header &header = makeHeader(number);
        const uint32_t slot = index & (PAGE_SIZE - 1);
        const uint32_t here = std::min(count, PAGE_SIZE - slot);
        occupy(header, number, here);
        if constexpr (HAS_VALUE) {
          try {
            std::uninitialized_fill_n(valueAt(index), here, value);
          } catch (...) {
            // The ones before stay, these are all gone again.
            forsake(header, number, here);
            throw;
          }
        }
        for (uint32_t bit = slot; bit < slot + here;) {
          const uint32_t width = std::min(64 - (bit % 64), slot + here - bit);
          const uint64_t bits = (width == 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1) << (bit % 64);
          [[maybe_unused]] const uint64_t before = header.mask[bit / 64].fetch_or(bits, std::memory_order_relaxed);
          assert((before & bits) == 0);
          markWord(header, bit / 64);
          bit += width;
        }
        index += here;
        count -= here;
      }
    }

    bool erase(uint32_t index) noexcept override {
      if (index >= INDEX_LIMIT) {
        return false;
      }
      const uint32_t number = index >> PAGE_BITS;
      Header *const found = findHeader(number);
      const uint32_t slot = index & (PAGE_SIZE - 1);
      if (found == nullptr || !has(*found, slot)) {
        return false;
      }
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
      } else if ((header.mask[slot / 64].fetch_and(~bit, std::memory_order_acq_rel) & bit) == 0) {
        return false;
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
        const uint32_t left = header->count.load(std::memory_order_relaxed) - removed;
        header->count.store(left, std::memory_order_relaxed);
        if (left == 0) {
          header->summary.store(0, std::memory_order_relaxed);
          if constexpr (HAS_VALUE) {
            park(*header, kill.number);
          } else {
            vacate(*header, kill.number);
          }
        }
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
        found += number != NO_PAGE && m_headers[number].count.load(std::memory_order_relaxed) == 0 &&
                 m_headers[number].started.load(std::memory_order_relaxed);
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
      return static_cast<size_t>(std::popcount(m_headerBlocks.load(std::memory_order_relaxed)));
    }

    // How many chunks of the values have their addresses reserved, see
    // CHUNK_SIZE. They stay until the column goes.
    [[nodiscard]] size_t chunks() const noexcept { return m_chunkCount.load(std::memory_order_relaxed); }

    // The headers come in blocks of this many pages.
    static constexpr uint32_t HEADER_BLOCK_PAGES = 64;
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

    static constexpr size_t HEADER_BLOCK_COUNT = PAGE_COUNT / HEADER_BLOCK_PAGES;
    static_assert(PAGE_COUNT % HEADER_BLOCK_PAGES == 0);
    static_assert(HEADER_BLOCK_COUNT <= 64, "one bit of m_headerBlocks for every block");
    static constexpr size_t HEADER_BLOCK_BYTES = sizeof(Header) * HEADER_BLOCK_PAGES;
    static constexpr size_t HEADER_BYTES = HEADER_BLOCK_BYTES * HEADER_BLOCK_COUNT;
    // A block gets its memory without touching its neighbours.
    static_assert(HEADER_BLOCK_BYTES % memory::OS_PAGE_SIZE == 0);

    static constexpr unsigned CHUNK_BITS = chunkBits<sizeof(T)>();
    static constexpr uint32_t CHUNK_PAGES = CHUNK_SIZE >> PAGE_BITS;
    static constexpr size_t CHUNK_COUNT = HAS_VALUE ? INDEX_LIMIT >> CHUNK_BITS : 0;
    static constexpr size_t CHUNK_BYTES = PAGE_BYTES * CHUNK_PAGES;
    // With room to move the values to where a huge page may start.
    static constexpr size_t CHUNK_RESERVED_BYTES = CHUNK_BYTES + memory::HUGE_PAGE_SIZE;

    // The OS may hand the values out in huge pages, each of which holds
    // several pages of the column or a part of one. A chunk starts at a huge
    // page; its last one may be cut short where the chunk is not a whole
    // number of them.
    static constexpr size_t CHUNK_REGIONS = (CHUNK_BYTES + memory::HUGE_PAGE_SIZE - 1) / memory::HUGE_PAGE_SIZE;
    static constexpr size_t REGION_COUNT = CHUNK_COUNT * CHUNK_REGIONS;
    // What m_regions says about a huge page.
    static constexpr uint32_t UNTOUCHED = 0;
    static constexpr uint32_t STARTING = 1;
    static constexpr uint32_t STARTED = 2;
    // An empty place among the spares.
    static constexpr uint32_t NO_PAGE = ~uint32_t{0};

    [[nodiscard]] static bool has(const Header &header, uint32_t slot) noexcept {
      return ((header.mask[slot / 64].load(std::memory_order_relaxed) >> (slot % 64)) & 1u) != 0;
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

    // The header of a page, null as long as no page of its block was used.
    [[nodiscard]] Header *findHeader(uint32_t number) const noexcept {
      const uint64_t blocks = m_headerBlocks.load(std::memory_order_acquire);
      return ((blocks >> (number / HEADER_BLOCK_PAGES)) & 1u) != 0 ? m_headers + number : nullptr;
    }

    // The same, with the block given its memory if it has none yet. Throws
    // std::bad_alloc if there is no memory for it.
    [[nodiscard]] Header &makeHeader(uint32_t number) {
      const uint64_t bit = uint64_t{1} << (number / HEADER_BLOCK_PAGES);
      if ((m_headerBlocks.load(std::memory_order_acquire) & bit) == 0) [[unlikely]] {
        startHeaderBlock(number / HEADER_BLOCK_PAGES);
      }
      return m_headers[number];
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
      memory::commit(m_headers + (block * HEADER_BLOCK_PAGES), HEADER_BLOCK_BYTES);
      m_headerBlocks.fetch_or(uint64_t{1} << block, std::memory_order_acq_rel);
    }

    // Where the component of an index is, made or not. Its chunk must be
    // reserved. Relaxed: a thread that may touch the component has seen the
    // page get its memory, and with it the chunk.
    [[nodiscard]] T *valueAt(uint32_t index) const noexcept {
      return m_chunks[index >> CHUNK_BITS].load(std::memory_order_relaxed) + (index & (CHUNK_SIZE - 1));
    }

    // The values of a page, null for a component without members and for a
    // page whose chunk was never reserved.
    [[nodiscard]] T *valuesOf(uint32_t number) const noexcept {
      if constexpr (HAS_VALUE) {
        T *const values = m_chunks[number / CHUNK_PAGES].load(std::memory_order_relaxed);
        return values != nullptr ? values + (size_t{number % CHUNK_PAGES} << PAGE_BITS) : nullptr;
      } else {
        return nullptr;
      }
    }

    // The values of a chunk, reserved if they are not yet. Throws
    // std::bad_alloc if the OS has no room for them. Out of line, as
    // makeHeaderBlock.
    [[gnu::noinline]] T *makeChunk(size_t chunk) {
      std::atomic<T *> &slot = m_chunks[chunk];
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
      m_chunkKept[chunk] = kept;
      slot.store(fresh, std::memory_order_release);
      // Only read when the column goes, after every thread is done with it.
      m_chunkMemory[chunk] = reserved;
      m_chunkCount.fetch_add(1, std::memory_order_relaxed);
      return fresh;
    }

    // Counts so many components more in a page, which is all there is to it
    // unless the page has no memory: the values get it then. A spare has it,
    // also for its first component.
    //
    // Other threads may add to the page and remove from it at the same time.
    // A thread that finds the memory missing waits here for the one that
    // fetches it, or that is taking it back, see vacate. One that finds it
    // there may use it: vacate marks it as gone before it looks at the count,
    // which it finds raised then.
    void occupy(Header &header, uint32_t number, uint32_t count) {
      header.count.fetch_add(count, std::memory_order_seq_cst);
      if (header.started.load(std::memory_order_seq_cst)) [[likely]] {
        return;
      }
      start(header, number, count);
    }

    // The rest of occupy, for the first components of a page. Out of line, as
    // makeHeaderBlock.
    [[gnu::noinline]] void start(Header &header, uint32_t number, uint32_t count) {
      const Busy busy{header};
      if (header.started.load(std::memory_order_relaxed)) {
        return;
      }
      if constexpr (HAS_VALUE) {
        try {
          startPage(number, header.vacated);
        } catch (...) {
          header.count.fetch_sub(count, std::memory_order_seq_cst);
          throw;
        }
      }
      m_pages.fetch_add(1, std::memory_order_relaxed);
      header.started.store(true, std::memory_order_seq_cst);
    }

    // Counts so many components less in a page, which are gone already.
    void forsake(Header &header, uint32_t number, uint32_t count) noexcept {
      if (header.count.fetch_sub(count, std::memory_order_seq_cst) == count) [[unlikely]] {
        // No word has a bit any more. A thread that gave the page a
        // component meanwhile may have found its word still in the summary
        // and left it as it was: the summary is made anew from the mask
        // then.
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
      Header &header = m_headers[number];
      header.parked.store(false, std::memory_order_seq_cst);
      vacate(header, number);
    }

    // A page has no component: its memory goes back, unless a thread has
    // added one since.
    //
    // Whoever adds one counts it first and looks for the memory after, and
    // here the memory is marked as gone first and the count looked at after.
    // So either the count seen here is not zero and the memory stays, or the
    // other thread finds the memory gone and waits in occupy until it is.
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
      if (header.count.load(std::memory_order_seq_cst) != 0) {
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
    // With small pages the memory of the page is its own, and it is fetched
    // in one go: left to the first accesses, the OS would be asked once for
    // every OS_PAGE_SIZE of it. Not in a chunk that memory::rangeCache kept,
    // until the page gave its memory back: the memory is there already, or
    // offered to the OS and still there unless it took it. Writing to it is
    // all it takes then, while fetching it in one go has the OS go through
    // every piece of it: 1.5 instead of 2.5 ms to create 100 000 entities
    // with 320 bytes of components each in small pages.
    void startPage(uint32_t number, bool vacated) {
      const size_t chunk = number / CHUNK_PAGES;
      auto *const base = reinterpret_cast<std::byte *>(makeChunk(chunk));
      // Within the chunk.
      const size_t offset = size_t{number % CHUNK_PAGES} * PAGE_BYTES;
      std::byte *const first = base + offset;
      memory::commit(first, PAGE_BYTES);
      const size_t firstRegion = (chunk * CHUNK_REGIONS) + (offset / memory::HUGE_PAGE_SIZE);
      const size_t lastRegion = (chunk * CHUNK_REGIONS) + ((offset + PAGE_BYTES - 1) / memory::HUGE_PAGE_SIZE);

      if (!hugePages()) {
        if (m_grows && (pages() + 1) * PAGE_BYTES >= HUGE_PAGE_COLUMN_BYTES) {
          moveToHugePages();
        } else {
          // Remembered for moveToHugePages. Whoever comes to the huge page
          // later finds it started.
          for (size_t region = firstRegion; region <= lastRegion; ++region) {
            m_regions[region].store(STARTED, std::memory_order_release);
          }
          if (vacated || !m_chunkKept[chunk]) {
            memory::populate(first, PAGE_BYTES);
          }
          return;
        }
      }

      for (size_t region = firstRegion; region <= lastRegion; ++region) {
        std::atomic<uint32_t> &state = m_regions[region];
        uint32_t seen = state.load(std::memory_order_acquire);
        if (seen == STARTED) [[likely]] {
          continue;
        }
        if (seen == UNTOUCHED &&
            state.compare_exchange_strong(seen, STARTING, std::memory_order_acq_rel, std::memory_order_acquire)) {
          // Within this page: the rest of the huge page belongs to others.
          const size_t touched = std::max(offset, (region - (chunk * CHUNK_REGIONS)) * memory::HUGE_PAGE_SIZE);
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
      for (size_t chunk = 0; chunk < CHUNK_COUNT; ++chunk) {
        T *const values = m_chunks[chunk].load(std::memory_order_acquire);
        if (values == nullptr) {
          continue;
        }
        memory::preferHugePages(values, CHUNK_BYTES);
        auto *const base = reinterpret_cast<std::byte *>(values);
        // Only the huge pages that lie in the chunk as a whole.
        for (size_t region = 0; (region + 1) * memory::HUGE_PAGE_SIZE <= CHUNK_BYTES; ++region) {
          if (m_regions[(chunk * CHUNK_REGIONS) + region].load(std::memory_order_acquire) == STARTED) {
            memory::collapse(base + (region * memory::HUGE_PAGE_SIZE), memory::HUGE_PAGE_SIZE);
          }
        }
      }
    }

    // Changes once at most, from small pages to huge ones, see Pages::Auto.
    std::atomic<bool> m_hugePages{true};
    bool m_grows = false;
    // The header of every page, reserved for all of them and given memory a
    // block of HEADER_BLOCK_PAGES at a time, when a page of the block is
    // first used; a bit of m_headerBlocks for every block that has it. On
    // Linux with vm.overcommit_memory=2 the whole range of HEADER_BYTES is
    // promised from the start, see vacate.
    Header *m_headers;
    std::atomic<uint64_t> m_headerBlocks{0};
    // The values of every chunk, at a huge page, null until the chunk is
    // reserved; and the range reserved for it, which they lie in.
    std::array<std::atomic<T *>, CHUNK_COUNT> m_chunks{};
    std::array<void *, CHUNK_COUNT> m_chunkMemory{};
    // Whether memory::rangeCache kept the range of a chunk, see startPage.
    std::array<bool, CHUNK_COUNT> m_chunkKept{};
    std::atomic<size_t> m_chunkCount{0};
    // Held while a chunk is reserved.
    std::mutex m_growMutex;
    // Whether the first access to a huge page of the values was made, by
    // chunk.
    std::array<std::atomic<uint32_t>, REGION_COUNT> m_regions{};
    std::atomic<size_t> m_pages{0};
    // The emptied pages that keep their memory, NO_PAGE for an empty place;
    // the next one takes the place m_nextSpare points to.
    std::array<std::atomic<uint32_t>, SPARE_PAGES> m_spares;
    std::atomic<uint32_t> m_nextSpare{0};
  };

} // namespace sc::ecs2::internal
