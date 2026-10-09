#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include <mimalloc.h>

#include "entity.hpp"

namespace sc::ecs2::internal {

  // One bit for every entity of a page.
  inline constexpr size_t PAGE_WORDS = PAGE_SIZE / 64;

  // What the world needs of a column without knowing its component.
  struct ColumnBase {
    virtual ~ColumnBase() = default;
    // False if there was no component at the index.
    virtual bool erase(uint32_t index) noexcept = 0;
  };

  // The components of one type, each at the index of its entity.
  //
  // The column is cut into pages. A page has memory from the first component
  // in it until the last one is gone, so a range of indices without the
  // component costs nothing, neither memory nor time to go through.
  //
  // Reading is safe from any number of threads. Adding and removing is safe
  // for threads that keep to different pages.
  template <typename T> class Column final : public ColumnBase {
    static_assert(std::is_trivially_destructible_v<T>, "a component is not destroyed one by one");
    static_assert(!std::is_const_v<T> && !std::is_reference_v<T>);

    // The pages are reached through directories, which are only there for the
    // index ranges in use: indices may be far apart.
    static constexpr unsigned DIRECTORY_BITS = 11;
    static constexpr size_t DIRECTORY_SIZE = size_t{1} << DIRECTORY_BITS;
    static constexpr size_t DIRECTORY_COUNT = size_t{1} << (INDEX_BITS - PAGE_BITS - DIRECTORY_BITS);

  public:
    // A component without members has no instances, only the bits.
    static constexpr bool HAS_VALUE = !std::is_empty_v<T>;

    struct Page {
      // Which entities of the page have the component.
      std::array<uint64_t, PAGE_WORDS> mask{};
      uint32_t count = 0;

      [[nodiscard]] T *values() noexcept {
        return reinterpret_cast<T *>(reinterpret_cast<std::byte *>(this) + VALUES_OFFSET);
      }
      [[nodiscard]] const T *values() const noexcept {
        return reinterpret_cast<const T *>(reinterpret_cast<const std::byte *>(this) + VALUES_OFFSET);
      }
      [[nodiscard]] bool has(uint32_t slot) const noexcept { return ((mask[slot / 64] >> (slot % 64)) & 1u) != 0; }
    };

    Column() = default;

    ~Column() override {
      for (std::atomic<Directory *> &entry : m_directories) {
        Directory *directory = entry.load(std::memory_order_relaxed);
        if (directory == nullptr) {
          continue;
        }
        for (std::atomic<Page *> &page : directory->pages) {
          mi_free(page.load(std::memory_order_relaxed));
        }
        delete directory;
      }
    }

    Column(const Column &) = delete;
    Column &operator=(const Column &) = delete;

    // The page with the given number, null if no entity of it has the
    // component.
    [[nodiscard]] Page *page(uint32_t number) const noexcept {
      const Directory *directory = m_directories[number >> DIRECTORY_BITS].load(std::memory_order_acquire);
      if (directory == nullptr) {
        return nullptr;
      }
      return directory->pages[number & (DIRECTORY_SIZE - 1)].load(std::memory_order_acquire);
    }

    [[nodiscard]] bool contains(uint32_t index) const noexcept {
      const Page *found = page(index >> PAGE_BITS);
      return found != nullptr && found->has(index & (PAGE_SIZE - 1));
    }

    // There must be a component at the index.
    [[nodiscard]] T &get(uint32_t index) const noexcept {
      Page *found = page(index >> PAGE_BITS);
      assert(found != nullptr && found->has(index & (PAGE_SIZE - 1)));
      return found->values()[index & (PAGE_SIZE - 1)];
    }

    // There must be no component at the index yet. Returns the new component,
    // or nothing for a component without members.
    template <typename... Args> decltype(auto) emplace(uint32_t index, Args &&...args) {
      Page &target = makePage(index >> PAGE_BITS);
      const uint32_t slot = index & (PAGE_SIZE - 1);
      assert(!target.has(slot));
      target.mask[slot / 64] |= uint64_t{1} << (slot % 64);
      ++target.count;
      if constexpr (HAS_VALUE) {
        return *::new (static_cast<void *>(target.values() + slot)) T{std::forward<Args>(args)...};
      }
    }

    bool erase(uint32_t index) noexcept override {
      const uint32_t number = index >> PAGE_BITS;
      Directory *directory = m_directories[number >> DIRECTORY_BITS].load(std::memory_order_acquire);
      if (directory == nullptr) {
        return false;
      }
      std::atomic<Page *> &entry = directory->pages[number & (DIRECTORY_SIZE - 1)];
      Page *found = entry.load(std::memory_order_acquire);
      const uint32_t slot = index & (PAGE_SIZE - 1);
      if (found == nullptr || !found->has(slot)) {
        return false;
      }
      found->mask[slot / 64] &= ~(uint64_t{1} << (slot % 64));
      if (--found->count == 0) {
        // Nothing left in it: the memory goes back.
        entry.store(nullptr, std::memory_order_release);
        mi_free(found);
        m_pages.fetch_sub(1, std::memory_order_relaxed);
      }
      return true;
    }

    // How many pages have memory.
    [[nodiscard]] size_t pages() const noexcept { return m_pages.load(std::memory_order_relaxed); }

  private:
    struct Directory {
      std::array<std::atomic<Page *>, DIRECTORY_SIZE> pages{};
    };

    static constexpr size_t VALUES_ALIGNMENT = alignof(T) > 64 ? alignof(T) : 64;
    static constexpr size_t VALUES_OFFSET = (sizeof(Page) + VALUES_ALIGNMENT - 1) / VALUES_ALIGNMENT * VALUES_ALIGNMENT;
    static constexpr size_t PAGE_BYTES = HAS_VALUE ? VALUES_OFFSET + sizeof(T) * PAGE_SIZE : sizeof(Page);

    Page &makePage(uint32_t number) {
      std::atomic<Directory *> &top = m_directories[number >> DIRECTORY_BITS];
      Directory *directory = top.load(std::memory_order_acquire);
      if (directory == nullptr) [[unlikely]] {
        // Threads that keep to different pages may still share a directory.
        auto *fresh = new Directory;
        if (top.compare_exchange_strong(directory, fresh, std::memory_order_acq_rel)) {
          directory = fresh;
        } else {
          delete fresh;
        }
      }
      std::atomic<Page *> &entry = directory->pages[number & (DIRECTORY_SIZE - 1)];
      Page *found = entry.load(std::memory_order_acquire);
      if (found == nullptr) [[unlikely]] {
        found = ::new (mi_malloc_aligned(PAGE_BYTES, VALUES_ALIGNMENT)) Page;
        entry.store(found, std::memory_order_release);
        m_pages.fetch_add(1, std::memory_order_relaxed);
      }
      return *found;
    }

    std::array<std::atomic<Directory *>, DIRECTORY_COUNT> m_directories{};
    std::atomic<size_t> m_pages{0};
  };

} // namespace sc::ecs2::internal
