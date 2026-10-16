#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

#include "entity.hpp"
#include "sc/memory/virtual_memory.hpp"

namespace sc::ecs2 {

  // Says that few entities have a component:
  //
  //   template <> inline constexpr bool sc::ecs2::rareComponent<Player> = true;
  //
  // Its column then gets memory in small pieces, a page of the column at a
  // time, instead of the huge pages that make going through many components
  // fast: a huge page would be held by a handful of components.
  template <typename Component> inline constexpr bool rareComponent = false;

  // What the OS backs the columns of a world with: World world{Pages::Small}.
  enum class Pages : uint8_t {
    // Huge pages where the OS has them. Fastest to go through, but a column
    // takes its memory in pieces of memory::HUGE_PAGE_SIZE, so a world with
    // few entities holds much more than its components need.
    Huge,
    // Only pages of memory::OS_PAGE_SIZE, for every component and not just
    // the rare ones. The memory follows the components closely.
    Small,
    // Huge pages for the components of at least HUGE_PAGE_COMPONENT_BYTES and
    // small ones for the rest. A huge page holds few large components, so
    // little of it is left unused, while their column would need a great many
    // small pages, each of them fetched from the OS on its own. With small
    // components it is the other way around.
    BySize,
    // Small pages for a column until its components take up
    // HUGE_PAGE_COLUMN_BYTES, huge ones from then on, also for what it has
    // already. A world with few entities gets the memory of small pages, one
    // with many the speed of huge ones.
    Adaptive,
  };

  // From which size on a component gets huge pages in a world of
  // Pages::BySize. At this size a huge page holds the components of 8 pages
  // of a column.
  inline constexpr size_t HUGE_PAGE_COMPONENT_BYTES = 64;

  // How much memory the pages of a column take before it moves to huge pages
  // in a world of Pages::Adaptive: as much as one of them holds. A column
  // that is filled without gaps then never holds more than a huge page it
  // does not need, and little of it has to be moved over.
  inline constexpr size_t HUGE_PAGE_COLUMN_BYTES = memory::HUGE_PAGE_SIZE;

} // namespace sc::ecs2

namespace sc::ecs2::internal {

  // One bit for every entity of a page.
  inline constexpr size_t PAGE_WORDS = PAGE_SIZE / 64;
  // How many pages the indices are cut into.
  inline constexpr size_t PAGE_COUNT = INDEX_LIMIT >> PAGE_BITS;

  // What the world needs of a column without knowing its component.
  struct ColumnBase {
    virtual ~ColumnBase() = default;
    // False if there was no component at the index.
    virtual bool erase(uint32_t index) noexcept = 0;
  };

  // The components of one type, each at the index of its entity.
  //
  // The column reserves the addresses for every index below INDEX_LIMIT, once,
  // and nothing else: the component of an index is always at the same place,
  // one page of the column right after the other. A page gets memory with the
  // first component in it and gives it back when the last one is gone, so a
  // range of indices without the component costs nothing, neither memory nor
  // time to go through.
  //
  // The pages lie without gaps on purpose. Memory that an allocator hands out
  // page by page has unused bytes between the pages, and with them parts of
  // the CPU cache that the column never gets to use.
  //
  // Reading is safe from any number of threads. Adding and removing is safe
  // for threads that keep to different indices, also next to threads that ask
  // whether an index has the component: what a page says about its entities
  // is changed in single steps that no thread sees half done.
  //
  // Several threads may also go for the same index, with tryEmplace and with
  // erase, in any mix: one of them does it and the others are told that they
  // did not. An index has the component from when it is made in full until
  // just before it is destroyed, for every thread that asks.
  template <typename T> class Column final : public ColumnBase {
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
      // not made in its place before it is gone.
      std::array<std::atomic<uint64_t>, PAGE_WORDS> making;
      std::atomic<uint32_t> count;
      // Whether the values have memory. Only changed by the thread that has
      // busy set.
      std::atomic<bool> started;
      // Set while a thread gives the values their memory or takes it back.
      std::atomic_flag busy;
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
      uint32_t count = 0;
      // Null for a component without members.
      T *values = nullptr;

      explicit operator bool() const noexcept { return mask != nullptr; }
    };

    // A rare component gets small pages whatever the world asks for.
    explicit Column(Pages pages = Pages::Huge)
        : m_memory(memory::reserve(TOTAL_BYTES)),
          m_hugePages(!rareComponent<T> &&
                      (pages == Pages::Huge || (pages == Pages::BySize && sizeof(T) >= HUGE_PAGE_COMPONENT_BYTES))),
          m_grows(!rareComponent<T> && pages == Pages::Adaptive) {
      auto *start = static_cast<std::byte *>(m_memory);
      m_headers = reinterpret_cast<Header *>(start);
      // Few enough to have memory from the start, all zero until used.
      // TODO: HEADER_BYTES is 4.25 MiB per column (sizeof(Header) = 1088,
      // measured with clang on Linux, times PAGE_COUNT 4096). On Linux the
      // commit costs nothing until touched, on Windows MEM_COMMIT counts
      // against the commit limit right away, for every column of every
      // world. The values also reserve sizeof(T) * INDEX_LIMIT of address
      // space: 1 GiB for a 64 B component, 16 GiB for 1 KiB, so large
      // components held by value run out of the 128 TiB user space after a
      // few thousand columns.
      // On Windows the values never give their commit back either:
      // memory::discard is DiscardVirtualMemory, which frees the RAM but keeps
      // the pages committed, so a column's commit charge stays at the most it
      // ever held until the column is destroyed. Only MEM_DECOMMIT lowers it.
      // On Linux with vm.overcommit_memory=2, MAP_NORESERVE is ignored and
      // memory::reserve counts the whole writable range against the commit
      // limit, so it fails after a few columns. Reserving with PROT_NONE and
      // making it writable in commit with mprotect would avoid that.
      memory::commit(m_headers, HEADER_BYTES);
      if constexpr (HAS_VALUE) {
        // Where a huge page may start: an address, not an offset, the range
        // itself may begin anywhere.
        const auto address = reinterpret_cast<uintptr_t>(start) + HEADER_BYTES;
        m_values = reinterpret_cast<T *>((address + memory::HUGE_PAGE_SIZE - 1) & ~(memory::HUGE_PAGE_SIZE - 1));
        if (hugePages()) {
          memory::preferHugePages(m_values, VALUE_BYTES);
        } else {
          memory::preferSmallPages(m_values, VALUE_BYTES);
        }
      }
    }

    ~Column() override {
      if constexpr (DESTROYS) {
        // The pages that have components, which are as many as have memory.
        size_t left = pages();
        for (uint32_t number = 0; left != 0 && number < PAGE_COUNT; ++number) {
          Header &header = m_headers[number];
          if (header.count.load(std::memory_order_relaxed) == 0) {
            continue;
          }
          --left;
          T *const values = valuesOf(number);
          for (uint32_t word = 0; word < PAGE_WORDS; ++word) {
            for (uint64_t mask = header.mask[word].load(std::memory_order_relaxed); mask != 0; mask &= mask - 1) {
              std::destroy_at(values + (word * 64) + static_cast<uint32_t>(std::countr_zero(mask)));
            }
          }
        }
      }
      memory::release(m_memory, TOTAL_BYTES);
    }

    Column(const Column &) = delete;
    Column &operator=(const Column &) = delete;

    // The page with the given number, nothing if no entity of it has the
    // component.
    [[nodiscard]] Page page(uint32_t number) const noexcept {
      assert(number < PAGE_COUNT);
      const Header &header = m_headers[number];
      const uint32_t count = header.count.load(std::memory_order_relaxed);
      if (count == 0) {
        return {};
      }
      return {.mask = header.mask.data(), .count = count, .values = valuesOf(number)};
    }

    [[nodiscard]] bool contains(uint32_t index) const noexcept {
      // Any index may be asked for, also that of an entity there never was.
      return index < INDEX_LIMIT && has(m_headers[index >> PAGE_BITS], index & (PAGE_SIZE - 1));
    }

    // There must be a component at the index.
    [[nodiscard]] T &get(uint32_t index) const noexcept {
      assert(contains(index));
      return m_values[index];
    }

    // There must be no component at the index yet, and no other thread that
    // adds or removes the component of this index. Returns the new component,
    // or nothing for a component without members. If making the component
    // throws, there is none at the index afterwards, as before.
    template <typename... Args> decltype(auto) emplace(uint32_t index, Args &&...args) {
      assert(index < INDEX_LIMIT);
      const uint32_t number = index >> PAGE_BITS;
      Header &header = m_headers[number];
      const uint32_t slot = index & (PAGE_SIZE - 1);
      assert(!has(header, slot));
      occupy(header, number, 1);
      if constexpr (HAS_VALUE) {
        T *made = nullptr;
        try {
          made = ::new (static_cast<void *>(m_values + index)) T{std::forward<Args>(args)...};
        } catch (...) {
          forsake(header, number, 1);
          throw;
        }
        // Only now: an index that has the component has one that was made.
        header.mask[slot / 64].fetch_or(uint64_t{1} << (slot % 64), std::memory_order_relaxed);
        return *made;
      } else {
        header.mask[slot / 64].fetch_or(uint64_t{1} << (slot % 64), std::memory_order_relaxed);
      }
    }

    // The same for an index that may have the component already, and that
    // other threads may be giving it or taking it from at the same time:
    // false if it had the component or another thread was first. The
    // component is that thread's then, with its arguments.
    template <typename... Args> bool tryEmplace(uint32_t index, [[maybe_unused]] Args &&...args) {
      assert(index < INDEX_LIMIT);
      const uint32_t number = index >> PAGE_BITS;
      Header &header = m_headers[number];
      const uint32_t slot = index & (PAGE_SIZE - 1);
      if (has(header, slot)) {
        return false;
      }
      // The index is this thread's to make the component for, or another
      // thread is at it.
      std::atomic<uint64_t> &making = header.making[slot / 64];
      const uint64_t bit = uint64_t{1} << (slot % 64);
      if ((making.fetch_or(bit, std::memory_order_acq_rel) & bit) != 0) {
        return false;
      }
      // Asked again: a thread may have made it and be done since.
      if (has(header, slot)) {
        making.fetch_and(~bit, std::memory_order_release);
        return false;
      }
      try {
        occupy(header, number, 1);
        if constexpr (HAS_VALUE) {
          try {
            ::new (static_cast<void *>(m_values + index)) T{std::forward<Args>(args)...};
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
      making.fetch_and(~bit, std::memory_order_release);
      return true;
    }

    // Says that so many components are about to be added. In a world of
    // Pages::Adaptive the column moves to huge pages now if they would take
    // it there, so that their memory is not fetched in small pages first and
    // moved over afterwards.
    void expect(size_t count) {
      if constexpr (HAS_VALUE) {
        if (m_grows && !hugePages() && pages() * PAGE_BYTES + count * sizeof(T) >= HUGE_PAGE_COLUMN_BYTES) {
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
        Header &header = m_headers[number];
        const uint32_t slot = index & (PAGE_SIZE - 1);
        const uint32_t here = std::min(count, PAGE_SIZE - slot);
        occupy(header, number, here);
        if constexpr (HAS_VALUE) {
          try {
            std::uninitialized_fill_n(m_values + index, here, value);
          } catch (...) {
            // The ones before stay, these are all gone again.
            forsake(header, number, here);
            throw;
          }
        }
        for (uint32_t bit = slot; bit < slot + here;) {
          const uint32_t width = std::min(64 - bit % 64, slot + here - bit);
          const uint64_t bits = (width == 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1) << (bit % 64);
          [[maybe_unused]] const uint64_t before = header.mask[bit / 64].fetch_or(bits, std::memory_order_relaxed);
          assert((before & bits) == 0);
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
      Header &header = m_headers[number];
      const uint32_t slot = index & (PAGE_SIZE - 1);
      if (!has(header, slot)) {
        return false;
      }
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
          std::destroy_at(m_values + index);
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

    // Whether the OS may hand the values out in huge pages.
    [[nodiscard]] bool hugePages() const noexcept { return m_hugePages.load(std::memory_order_relaxed); }

    // How many pages have memory.
    [[nodiscard]] size_t pages() const noexcept { return m_pages.load(std::memory_order_relaxed); }

  private:
    // The values of a page are a whole number of memory::OS_PAGE_SIZE, so a
    // page gets and gives back its memory without touching its neighbours.
    // Where the pages of the OS are larger and a small component makes the
    // two share one, the shared ones stay with the column: memory::discard
    // only gives back what belongs to the page alone.
    static constexpr size_t PAGE_BYTES = sizeof(T) * PAGE_SIZE;
    static_assert(!HAS_VALUE || PAGE_BYTES % memory::OS_PAGE_SIZE == 0);

    static constexpr size_t HEADER_BYTES = sizeof(Header) * PAGE_COUNT;
    static constexpr size_t VALUE_BYTES = HAS_VALUE ? PAGE_BYTES * PAGE_COUNT : 0;
    // With room to move the values to where a huge page may start.
    static constexpr size_t TOTAL_BYTES = HEADER_BYTES + (HAS_VALUE ? memory::HUGE_PAGE_SIZE + VALUE_BYTES : 0);

    static_assert(HEADER_BYTES % memory::OS_PAGE_SIZE == 0);

    // The OS may hand the values out in huge pages, each of which holds
    // several pages of the column or a part of one.
    static constexpr size_t REGION_COUNT = HAS_VALUE ? VALUE_BYTES / memory::HUGE_PAGE_SIZE : 1;
    static_assert(VALUE_BYTES % memory::HUGE_PAGE_SIZE == 0);
    // What m_regions says about a huge page.
    static constexpr uint32_t UNTOUCHED = 0;
    static constexpr uint32_t STARTING = 1;
    static constexpr uint32_t STARTED = 2;

    [[nodiscard]] static bool has(const Header &header, uint32_t slot) noexcept {
      return ((header.mask[slot / 64].load(std::memory_order_relaxed) >> (slot % 64)) & 1u) != 0;
    }

    [[nodiscard]] T *valuesOf(uint32_t number) const noexcept {
      if constexpr (HAS_VALUE) {
        return m_values + (size_t{number} << PAGE_BITS);
      } else {
        return nullptr;
      }
    }

    // Counts so many components more in a page, which is all there is to it
    // unless they are its first ones: the values get their memory then.
    //
    // Other threads may add to the page and remove from it at the same time.
    // A thread that finds the memory missing waits here for the one that
    // fetches it, or that is taking it back, see vacate.
    void occupy(Header &header, uint32_t number, uint32_t count) {
      if (header.count.fetch_add(count, std::memory_order_seq_cst) != 0 &&
          header.started.load(std::memory_order_seq_cst)) [[likely]] {
        return;
      }
      const Busy busy{header};
      if (header.started.load(std::memory_order_relaxed)) {
        return;
      }
      if constexpr (HAS_VALUE) {
        try {
          startPage(number);
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
        vacate(header, number);
      }
    }

    // The last component of a page was removed: its memory goes back, unless
    // a thread has added one since.
    //
    // Whoever adds one counts it first and looks for the memory after, and
    // here the memory is marked as gone first and the count looked at after.
    // So either the count seen here is not zero and the memory stays, or the
    // other thread finds the memory gone and waits in occupy until it is.
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
        memory::discard(valuesOf(number), PAGE_BYTES);
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
    // every OS_PAGE_SIZE of it.
    void startPage(uint32_t number) {
      auto *const first = reinterpret_cast<std::byte *>(valuesOf(number));
      memory::commit(first, PAGE_BYTES);
      const size_t offset = static_cast<size_t>(first - reinterpret_cast<std::byte *>(m_values));
      const size_t lastRegion = (offset + PAGE_BYTES - 1) / memory::HUGE_PAGE_SIZE;

      if (!hugePages()) {
        if (m_grows && (pages() + 1) * PAGE_BYTES >= HUGE_PAGE_COLUMN_BYTES) {
          moveToHugePages();
        } else {
          // Remembered for moveToHugePages. Whoever comes to the huge page
          // later finds it started.
          for (size_t region = offset / memory::HUGE_PAGE_SIZE; region <= lastRegion; ++region) {
            m_regions[region].store(STARTED, std::memory_order_release);
          }
          memory::populate(first, PAGE_BYTES);
          return;
        }
      }

      for (size_t region = offset / memory::HUGE_PAGE_SIZE; region <= lastRegion; ++region) {
        std::atomic<uint32_t> &state = m_regions[region];
        uint32_t seen = state.load(std::memory_order_acquire);
        if (seen == STARTED) [[likely]] {
          continue;
        }
        if (seen == UNTOUCHED &&
            state.compare_exchange_strong(seen, STARTING, std::memory_order_acq_rel, std::memory_order_acquire)) {
          // Within this page: the rest of the huge page belongs to others.
          const size_t touched = std::max(offset, region * memory::HUGE_PAGE_SIZE);
          *reinterpret_cast<volatile std::byte *>(reinterpret_cast<std::byte *>(m_values) + touched) = std::byte{0};
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
      memory::preferHugePages(m_values, VALUE_BYTES);
      auto *const values = reinterpret_cast<std::byte *>(m_values);
      for (size_t region = 0; region < REGION_COUNT; ++region) {
        if (m_regions[region].load(std::memory_order_acquire) == STARTED) {
          memory::collapse(values + region * memory::HUGE_PAGE_SIZE, memory::HUGE_PAGE_SIZE);
        }
      }
    }

    void *m_memory = nullptr;
    // Changes once at most, from small pages to huge ones, see Pages::Adaptive.
    std::atomic<bool> m_hugePages{true};
    bool m_grows = false;
    Header *m_headers = nullptr;
    // The component of every index, at that index.
    T *m_values = nullptr;
    // Whether the first access to a huge page of the values was made.
    std::array<std::atomic<uint32_t>, REGION_COUNT> m_regions{};
    std::atomic<size_t> m_pages{0};
  };

} // namespace sc::ecs2::internal
