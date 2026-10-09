#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
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
  // for threads that keep to different pages.
  template <typename T> class Column final : public ColumnBase {
    static_assert(std::is_trivially_destructible_v<T>, "a component is not destroyed one by one");
    static_assert(!std::is_const_v<T> && !std::is_reference_v<T>);
    static_assert(alignof(T) <= memory::OS_PAGE_SIZE);

    // What the column knows about a page, apart from its values. On cache
    // lines of its own: threads that keep to different pages share nothing.
    struct alignas(64) Header {
      // Which entities of the page have the component.
      std::array<uint64_t, PAGE_WORDS> mask;
      uint32_t count;
    };

  public:
    // A component without members has no instances, only the bits.
    static constexpr bool HAS_VALUE = !std::is_empty_v<T>;
    // Whether the OS may hand the values out in huge pages.
    static constexpr bool HUGE_PAGES = !rareComponent<T>;

    // A page that has components, or nothing.
    struct Page {
      // Which entities of the page have the component. Null for no page.
      const uint64_t *mask = nullptr;
      uint32_t count = 0;
      // Null for a component without members.
      T *values = nullptr;

      explicit operator bool() const noexcept { return mask != nullptr; }
    };

    Column() : m_memory(memory::reserve(TOTAL_BYTES)) {
      auto *start = static_cast<std::byte *>(m_memory);
      m_headers = reinterpret_cast<Header *>(start);
      // Few enough to have memory from the start, all zero until used.
      memory::commit(m_headers, HEADER_BYTES);
      if constexpr (HAS_VALUE) {
        // Where a huge page may start: an address, not an offset, the range
        // itself may begin anywhere.
        const auto address = reinterpret_cast<uintptr_t>(start) + HEADER_BYTES;
        m_values = reinterpret_cast<T *>((address + memory::HUGE_PAGE_SIZE - 1) & ~(memory::HUGE_PAGE_SIZE - 1));
        if (HUGE_PAGES) {
          memory::preferHugePages(m_values, VALUE_BYTES);
        } else {
          memory::preferSmallPages(m_values, VALUE_BYTES);
        }
      }
    }

    ~Column() override { memory::release(m_memory, TOTAL_BYTES); }

    Column(const Column &) = delete;
    Column &operator=(const Column &) = delete;

    // The page with the given number, nothing if no entity of it has the
    // component.
    [[nodiscard]] Page page(uint32_t number) const noexcept {
      assert(number < PAGE_COUNT);
      const Header &header = m_headers[number];
      if (header.count == 0) {
        return {};
      }
      return {.mask = header.mask.data(), .count = header.count, .values = valuesOf(number)};
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

    // There must be no component at the index yet. Returns the new component,
    // or nothing for a component without members.
    template <typename... Args> decltype(auto) emplace(uint32_t index, Args &&...args) {
      assert(index < INDEX_LIMIT);
      const uint32_t number = index >> PAGE_BITS;
      Header &header = m_headers[number];
      const uint32_t slot = index & (PAGE_SIZE - 1);
      assert(!has(header, slot));
      if (header.count == 0) [[unlikely]] {
        if constexpr (HAS_VALUE) {
          startPage(number);
        }
        m_pages.fetch_add(1, std::memory_order_relaxed);
      }
      header.mask[slot / 64] |= uint64_t{1} << (slot % 64);
      ++header.count;
      if constexpr (HAS_VALUE) {
        return *::new (static_cast<void *>(m_values + index)) T{std::forward<Args>(args)...};
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
      header.mask[slot / 64] &= ~(uint64_t{1} << (slot % 64));
      if (--header.count == 0) {
        // Nothing left in it: the memory goes back.
        if constexpr (HAS_VALUE) {
          memory::discard(valuesOf(number), PAGE_BYTES);
        }
        m_pages.fetch_sub(1, std::memory_order_relaxed);
      }
      return true;
    }

    // How many pages have memory.
    [[nodiscard]] size_t pages() const noexcept { return m_pages.load(std::memory_order_relaxed); }

  private:
    // The values of a page are a whole number of pages of the OS, so a page
    // gets and gives back its memory without touching its neighbours.
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
      return ((header.mask[slot / 64] >> (slot % 64)) & 1u) != 0;
    }

    [[nodiscard]] T *valuesOf(uint32_t number) const noexcept {
      if constexpr (HAS_VALUE) {
        return m_values + (size_t{number} << PAGE_BITS);
      } else {
        return nullptr;
      }
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
    void startPage(uint32_t number) {
      auto *const first = reinterpret_cast<std::byte *>(valuesOf(number));
      memory::commit(first, PAGE_BYTES);
      if constexpr (!HUGE_PAGES) {
        // Nothing to share: the memory of the page is its own.
        return;
      }

      const size_t offset = static_cast<size_t>(first - reinterpret_cast<std::byte *>(m_values));
      const size_t lastRegion = (offset + PAGE_BYTES - 1) / memory::HUGE_PAGE_SIZE;
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

    void *m_memory = nullptr;
    Header *m_headers = nullptr;
    // The component of every index, at that index.
    T *m_values = nullptr;
    // Whether the first access to a huge page of the values was made.
    std::array<std::atomic<uint32_t>, REGION_COUNT> m_regions{};
    std::atomic<size_t> m_pages{0};
  };

} // namespace sc::ecs2::internal
