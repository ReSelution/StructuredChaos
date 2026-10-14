#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "sc/threading/cpu_relax.hpp"

namespace sc::threading {

  namespace internal {
    // Memory of the operating system for the ring, outside of the header so
    // that it does not pull in <windows.h>. Throws std::bad_alloc on failure.
    [[nodiscard]] void *mapRingMemory(size_t bytes);
    void unmapRingMemory(void *memory, size_t bytes) noexcept;
    // Gives the pages that lie completely inside the range back to the system.
    // On Linux they read as zero once they are touched again. On Windows they
    // hold either zeros or their old content, so the content must be valid
    // either way: the queue only discards slots whose state is 0 already.
    void discardRingMemory(void *memory, size_t bytes) noexcept;
  } // namespace internal

  // Bounded multi-producer/multi-consumer queue on a ring of slots.
  //
  // A position in the ring is reused once per lap. The slots are split into
  // granules and every granule carries the lap it currently serves: a producer
  // may only write a slot, and a consumer may only read it, while the lap of its
  // granule matches the lap of their position. The granule moves on to the next
  // lap once all of its slots were popped, so a slot is never shared between two
  // laps.
  //
  // Queues spanning several blocks use one block as granule and hand the memory
  // of a fully popped block back to the OS. A producer then waits until the
  // whole block of the previous lap was popped, which can keep up to one block
  // of capacity unused. Smaller queues use one slot per granule and keep their
  // memory.
  //
  // The whole ring is mapped up front. On Linux that is address space only
  // until a slot is written; on Windows it counts against the commit limit.
  template<typename T, size_t Capacity = 4096>
  class RingQueue {

    struct Slot {
      alignas(64) std::atomic<size_t> state{0};
      alignas(alignof(T)) char storage[sizeof(T)];
    };

  public:
    RingQueue() :
        m_laps(std::make_unique<std::atomic<size_t>[]>(NUM_GRANULES)),
        m_pops(RECLAIM ? std::make_unique<std::atomic<size_t>[]>(NUM_GRANULES) : nullptr),
        m_rawMem(internal::mapRingMemory(TOTAL_BYTES)), m_slots(static_cast<Slot *>(m_rawMem)) {}

    ~RingQueue() noexcept {
      size_t h = m_head.load(std::memory_order_relaxed);
      const size_t t = m_tail.load(std::memory_order_relaxed);
      while (h < t) {
        Slot *slot = &m_slots[h & SLOT_MASK];
        if (slot->state.load(std::memory_order_relaxed) == 1) {
          std::launder(reinterpret_cast<T *>(&slot->storage))->~T();
        }
        h++;
      }
      internal::unmapRingMemory(m_rawMem, TOTAL_BYTES);
    }

    RingQueue(const RingQueue &) = delete;
    RingQueue &operator=(const RingQueue &) = delete;

    // Blocks (spinning) while the queue is full.
    template<typename... Args>
    void emplace(Args &&...args) {
      if constexpr (std::is_nothrow_constructible_v<T, Args...>) {
        place(std::forward<Args>(args)...);
      } else {
        // A slot that was taken and never filled would stop the queue for
        // good, so a constructor that may throw runs before a slot is taken.
        static_assert(std::is_nothrow_move_constructible_v<T>);
        T item(std::forward<Args>(args)...);
        place(std::move(item));
      }
    }

    void push(T &&t) { emplace(std::move(t)); }

    // workerId only staggers the back-off of competing consumers.
    bool tryPop(T &t, int32_t workerId) {
      size_t pos = m_head.load(std::memory_order_relaxed);
      Slot *slot = nullptr;

      while (true) {
        slot = &m_slots[pos & SLOT_MASK];

        // A set state alone is not enough: it may still belong to the previous
        // lap while another consumer is moving that item out.
        if (m_laps[granuleOf(pos)].load(std::memory_order_acquire) != lapOf(pos) ||
            slot->state.load(std::memory_order_acquire) == 0) [[unlikely]] {
          return false;
        }

        if (m_head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) [[likely]] {
          break;
        }

        const int32_t spins = 2 + (workerId & 3);
        for (int32_t i = 0; i < spins; ++i) {
          cpuRelax();
        }
      }

      T *item = std::launder(reinterpret_cast<T *>(&slot->storage));
      t = std::move(*item);
      item->~T();

      slot->state.store(0, std::memory_order_release);
      release(pos);
      return true;
    }

    [[nodiscard]] size_t size() const {
      const auto h = m_head.load(std::memory_order_relaxed);
      const auto t = m_tail.load(std::memory_order_relaxed);
      const auto diff = static_cast<ptrdiff_t>(t - h);
      return static_cast<size_t>(std::max<ptrdiff_t>(0, diff));
    }

    [[nodiscard]] bool empty() const { return size() == 0; }

    static constexpr size_t numBlocks() { return NUM_BLOCKS; }

    static constexpr size_t slotsPerBlock() { return SLOTS_PER_BLOCK; }

  private:
    template<typename... Args>
    void place(Args &&...args) noexcept {
      const size_t pos = m_tail.fetch_add(1, std::memory_order_relaxed);

      const std::atomic<size_t> &lap = m_laps[granuleOf(pos)];
      while (lap.load(std::memory_order_acquire) != lapOf(pos)) {
        cpuRelax();
      }

      Slot *slot = &m_slots[pos & SLOT_MASK];
      ::new (static_cast<void *>(&slot->storage)) T(std::forward<Args>(args)...);
      slot->state.store(1, std::memory_order_release);
    }

    // Marks the slot at pos as popped and moves its granule on to the next lap
    // once every slot of the granule was popped.
    void release(size_t pos) {
      const size_t granule = granuleOf(pos);

      if constexpr (RECLAIM) {
        if (m_pops[granule].fetch_add(1, std::memory_order_acq_rel) + 1 != GRANULE_SLOTS) {
          return;
        }
        // Only the last pop of a lap gets here, and no producer touches the
        // granule before its lap is advanced below.
        m_pops[granule].store(0, std::memory_order_relaxed);
        internal::discardRingMemory(static_cast<char *>(m_rawMem) + (granule * GRANULE_SLOTS * sizeof(Slot)),
                                    GRANULE_SLOTS * sizeof(Slot));
      }

      m_laps[granule].store(lapOf(pos) + 1, std::memory_order_release);
    }

    [[nodiscard]] static constexpr size_t lapOf(size_t pos) noexcept { return pos >> CAP_SHIFT; }

    [[nodiscard]] static constexpr size_t granuleOf(size_t pos) noexcept { return (pos & SLOT_MASK) >> GRANULE_SHIFT; }

    static constexpr size_t REAL_CAP = std::bit_ceil(Capacity);
    static constexpr size_t SLOT_MASK = REAL_CAP - 1;
    static constexpr size_t CAP_SHIFT = std::countr_zero(REAL_CAP);
    static constexpr size_t TOTAL_BYTES = REAL_CAP * sizeof(Slot);

    // The unit of reclaiming, independent of the page size of the system; the
    // pages that lie inside a block are given back.
    static constexpr size_t BLOCK_BYTES = size_t{64} * 1024;
    // Rounded down to a power of two, at least one slot.
    static constexpr size_t SLOTS_PER_BLOCK =
        std::min<size_t>(std::bit_floor(std::max<size_t>(BLOCK_BYTES / sizeof(Slot), 1)), REAL_CAP);
    static constexpr size_t NUM_BLOCKS = REAL_CAP / SLOTS_PER_BLOCK;

    static constexpr bool RECLAIM = NUM_BLOCKS > 1;
    static constexpr size_t GRANULE_SLOTS = RECLAIM ? SLOTS_PER_BLOCK : 1;
    static constexpr size_t GRANULE_SHIFT = std::countr_zero(GRANULE_SLOTS);
    static constexpr size_t NUM_GRANULES = REAL_CAP / GRANULE_SLOTS;

    // Before the ring, so that they are freed again if mapping it fails.
    // Lap each granule currently serves.
    std::unique_ptr<std::atomic<size_t>[]> m_laps;
    // Slots popped per granule in its current lap, only used when reclaiming.
    std::unique_ptr<std::atomic<size_t>[]> m_pops;

    void *m_rawMem = nullptr;
    Slot *m_slots = nullptr;

    alignas(64) std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};
  };

} // namespace sc::threading
