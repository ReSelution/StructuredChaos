#pragma once

#include <array>
#include <cstddef>
#include <mutex>

#include "sc/memory/virtual_memory.hpp"

namespace sc::memory {

  // Keeps reserved ranges that were given back, together with the memory
  // they have, for the next reservation of the same size. The memory is
  // offered to the OS meanwhile, see memory::offer: it takes it if it needs
  // it elsewhere, and leaves it in the range otherwise.
  //
  // Memory fresh from the OS is cleared by it on the first access, which
  // costs about as much as writing it: a world of sc::ecs2 that was made
  // anew for each run spent 38 % of creating 100 000 entities with 320 bytes
  // of components each in the kernel clearing huge pages, while libraries
  // that allocate with mimalloc get the memory of the world before back
  // without that (tests/ecs_compare, flecs: 2.2 ms, 15.4 ms when mimalloc
  // gives freed memory back at once). Offered, a range in huge pages was as
  // fast to use again as one that was simply kept: 0.9 to 1.0 ms to create
  // those entities with the columns of the world before. In small pages it
  // took 1.5 instead of 1.0 ms, as every piece is written to for the first
  // time again in the eyes of the OS; still far from the 9 ms with fresh
  // memory.
  //
  // A range comes back as it was given: what is in it is not defined, and it
  // may be backed in huge pages or small ones, whatever the one before
  // asked for. Only a range of exactly the size asked for is handed out, so
  // that it is given back to the OS with the size it was reserved with.
  //
  // Safe from any number of threads.
  class RangeCache {
  public:
    // How many bytes of ranges are kept at most. They count in full, as the
    // memory they may have. Enough for the columns of a world of 100 000
    // entities with 320 bytes of components each, 42 MiB.
    static constexpr size_t DEFAULT_LIMIT = size_t{64} << 20;
    // How many ranges are kept at most.
    static constexpr size_t MAX_RANGES = 64;

    explicit RangeCache(size_t limit = DEFAULT_LIMIT) noexcept : m_limit(limit) {}

    ~RangeCache() { trim(); }

    RangeCache(const RangeCache &) = delete;
    RangeCache &operator=(const RangeCache &) = delete;

    // A kept range of the given size, the one given back last, or a new one
    // without memory, as memory::reserve. Throws std::bad_alloc if the OS has
    // no room for a new one.
    [[nodiscard]] void *reserve(size_t bytes) {
      bool kept = false;
      return reserve(bytes, kept);
    }

    // The same, and whether the range was kept: it may have memory then.
    [[nodiscard]] void *reserve(size_t bytes, bool &kept) {
      kept = true;
      {
        const std::scoped_lock lock{m_mutex};
        for (size_t i = m_count; i-- != 0;) {
          if (m_ranges[i].bytes == bytes) {
            void *const address = m_ranges[i].address;
            // The others keep their order.
            for (size_t j = i + 1; j < m_count; ++j) {
              m_ranges[j - 1] = m_ranges[j];
            }
            --m_count;
            m_bytes -= bytes;
            return address;
          }
        }
      }
      kept = false;
      return memory::reserve(bytes);
    }

    // Keeps the range, with the address and size it was reserved with. The
    // ones kept longest go back to the OS to make room for it; the range
    // itself only if it is larger than the limit. Keeping the new one and
    // not the old: in a process that went through worlds of other
    // components first, the cache was full of ranges of sizes nobody asked
    // for any more, and the columns of the next world got nothing from it
    // (tests/ecs_compare run in full, 2.8 ms to create the entities of
    // HEAVY_WORLD, as without the cache).
    void release(void *address, size_t bytes) noexcept {
      // Before it is kept: once it is, another thread may take it and write
      // to it, which the offer would undo.
      memory::offer(address, bytes);
      std::array<Range, MAX_RANGES> gone{};
      size_t count = 0;
      {
        const std::scoped_lock lock{m_mutex};
        if (bytes <= m_limit) {
          count = dropOldest(m_limit - bytes, MAX_RANGES - 1, gone);
          m_ranges[m_count++] = {.address = address, .bytes = bytes};
          m_bytes += bytes;
        } else {
          gone[count++] = {.address = address, .bytes = bytes};
        }
      }
      for (size_t i = 0; i < count; ++i) {
        memory::release(gone[i].address, gone[i].bytes);
      }
    }

    // Gives every kept range back to the OS.
    void trim() noexcept { shrink(0); }

    // Changes the limit, and gives back what is over it now.
    void limit(size_t bytes) noexcept {
      {
        const std::scoped_lock lock{m_mutex};
        m_limit = bytes;
      }
      shrink(bytes);
    }

    [[nodiscard]] size_t limit() const noexcept {
      const std::scoped_lock lock{m_mutex};
      return m_limit;
    }

    // How many ranges are kept, and how many bytes they have together.
    [[nodiscard]] size_t ranges() const noexcept {
      const std::scoped_lock lock{m_mutex};
      return m_count;
    }
    [[nodiscard]] size_t bytes() const noexcept {
      const std::scoped_lock lock{m_mutex};
      return m_bytes;
    }

  private:
    struct Range {
      void *address = nullptr;
      size_t bytes = 0;
    };

    // Gives back the ranges kept longest until no more than the given bytes
    // are kept. The OS is asked outside the lock: unmapping takes the lock
    // of the process' mappings, which page faults wait for.
    void shrink(size_t keep) noexcept {
      std::array<Range, MAX_RANGES> gone{};
      size_t count = 0;
      {
        const std::scoped_lock lock{m_mutex};
        count = dropOldest(keep, MAX_RANGES, gone);
      }
      for (size_t i = 0; i < count; ++i) {
        memory::release(gone[i].address, gone[i].bytes);
      }
    }

    // Takes the ranges kept longest out until no more than the given bytes
    // and ranges are left, into gone, for the OS to get outside the lock.
    // Returns how many. The lock must be held.
    size_t dropOldest(size_t keepBytes, size_t keepRanges, std::array<Range, MAX_RANGES> &gone) noexcept {
      size_t first = 0;
      while (first < m_count && (m_bytes > keepBytes || m_count - first > keepRanges)) {
        m_bytes -= m_ranges[first].bytes;
        gone[first] = m_ranges[first];
        ++first;
      }
      for (size_t i = first; i < m_count; ++i) {
        m_ranges[i - first] = m_ranges[i];
      }
      m_count -= first;
      return first;
    }

    mutable std::mutex m_mutex;
    // In the order they were given back.
    std::array<Range, MAX_RANGES> m_ranges{};
    size_t m_count = 0;
    size_t m_bytes = 0;
    size_t m_limit;
  };

  // The one of the process, which the columns of sc::ecs2 take the ranges of
  // their values from. Never destroyed: a column that goes after the end of
  // main, with a world that is a static, still finds it. The OS takes back
  // what is kept when the process ends.
  [[nodiscard]] inline RangeCache &rangeCache() noexcept {
    static RangeCache &cache = *new RangeCache{};
    return cache;
  }

} // namespace sc::memory
