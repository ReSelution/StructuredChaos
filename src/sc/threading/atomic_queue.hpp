#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#if defined(_WIN32)
#include <intrin.h>
#include <windows.h>
#else
#include <sys/mman.h>
#include <x86intrin.h>
#endif

namespace sc::threading {

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
template <typename T, size_t Capacity = 4096> class AtomicQueue {

  struct Slot {
    alignas(64) std::atomic<size_t> state{0};
    alignas(alignof(T)) char storage[sizeof(T)];
  };

public:
  AtomicQueue() {
    m_laps = std::make_unique<std::atomic<size_t>[]>(NUM_GRANULES);
    if constexpr (RECLAIM) {
      m_pops = std::make_unique<std::atomic<size_t>[]>(NUM_GRANULES);
    }
#if defined(_WIN32)
    m_rawMem = VirtualAlloc(nullptr, TOTAL_BYTES, MEM_RESERVE | MEM_COMMIT,
                            PAGE_READWRITE);
#else
    m_rawMem = mmap(nullptr, TOTAL_BYTES, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    m_slots = reinterpret_cast<Slot *>(m_rawMem);
  }

  ~AtomicQueue() noexcept {
    size_t h = m_head.load(std::memory_order_relaxed);
    size_t t = m_tail.load(std::memory_order_relaxed);
    while (h < t) {
      Slot *slot = &m_slots[h & SLOT_MASK];
      if (slot->state.load(std::memory_order_relaxed) == 1) {
        auto *slot_ptr = reinterpret_cast<T *>(&slot->storage);
        slot_ptr->~T();
      }
      h++;
    }

#if defined(_WIN32)
    VirtualFree(m_rawMem, 0, MEM_RELEASE);
#else
    munmap(m_rawMem, TOTAL_BYTES);
#endif
  }

  AtomicQueue(const AtomicQueue &) = delete;
  AtomicQueue &operator=(const AtomicQueue &) = delete;

  // Blocks (spinning) while the queue is full.
  template <typename... Args> void emplace(Args &&...args) {
    const size_t pos = m_tail.fetch_add(1, std::memory_order_relaxed);

    const std::atomic<size_t> &lap = m_laps[granule_of(pos)];
    while (lap.load(std::memory_order_acquire) != lap_of(pos)) {
      pause();
    }

    Slot *slot = &m_slots[pos & SLOT_MASK];
    auto *item_ptr = reinterpret_cast<T *>(&slot->storage);
    ::new (static_cast<void *>(item_ptr)) T(std::forward<Args>(args)...);
    slot->state.store(1, std::memory_order_release);
  }

  void push(T &&t) { emplace(std::move(t)); }

  bool try_pop(T &t, int worker_id) {

    size_t pos = m_head.load(std::memory_order_relaxed);
    Slot *slot = nullptr;

    while (true) {
      slot = &m_slots[pos & SLOT_MASK];

      // A set state alone is not enough: it may still belong to the previous
      // lap while another consumer is moving that item out.
      if (m_laps[granule_of(pos)].load(std::memory_order_acquire) !=
              lap_of(pos) ||
          slot->state.load(std::memory_order_acquire) == 0) [[unlikely]] {
        return false;
      }

      if (m_head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
          [[likely]] {
        break;
      }

      const int spins = 2 + (worker_id & 3);
      for (int i = 0; i < spins; ++i) {
        pause();
      }
    }

    auto *item_ptr = reinterpret_cast<T *>(&slot->storage);
    t = std::move(*item_ptr);
    item_ptr->~T();

    slot->state.store(0, std::memory_order_release);
    release(pos);
    return true;
  }

  size_t size() const {
    const auto h = m_head.load(std::memory_order_relaxed);
    const auto t = m_tail.load(std::memory_order_relaxed);
    const auto diff = static_cast<ptrdiff_t>(t - h);
    return static_cast<size_t>(std::max<ptrdiff_t>(0, diff));
  }

  bool empty() const { return !size(); }

  constexpr static size_t num_blocks() { return NUM_BLOCKS; }

  constexpr static size_t slots_per_block() { return SLOTS_PER_BLOCK; }

private:
  static void pause() {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#elif defined(__ARM_ARCH) || defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
  }

  // Marks the slot at pos as popped and moves its granule on to the next lap
  // once every slot of the granule was popped.
  void release(size_t pos) {
    const size_t granule = granule_of(pos);

    if constexpr (RECLAIM) {
      if (m_pops[granule].fetch_add(1, std::memory_order_acq_rel) + 1 !=
          GRANULE_SLOTS) {
        return;
      }
      // Only the last pop of a lap gets here, and no producer touches the
      // granule before its lap is advanced below.
      m_pops[granule].store(0, std::memory_order_relaxed);
      discard(granule);
    }

    m_laps[granule].store(lap_of(pos) + 1, std::memory_order_release);
  }

  // Hands the pages that lie completely inside the granule back to the OS.
  void discard(size_t granule) {
    const size_t begin = granule * GRANULE_SLOTS * sizeof(Slot);
    const size_t end = begin + GRANULE_SLOTS * sizeof(Slot);
    const size_t first_page = (begin + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    const size_t last_page = end & ~(PAGE_SIZE - 1);
    if (first_page >= last_page) {
      return;
    }

    void *const address = static_cast<char *>(m_rawMem) + first_page;
    const size_t length = last_page - first_page;
#if defined(_WIN32)
    DiscardVirtualMemory(address, length);
#else
    madvise(address, length, MADV_DONTNEED);
#endif
  }

  [[nodiscard]] static constexpr size_t lap_of(size_t pos) noexcept {
    return pos >> CAP_SHIFT;
  }

  [[nodiscard]] static constexpr size_t granule_of(size_t pos) noexcept {
    return (pos & SLOT_MASK) >> GRANULE_SHIFT;
  }

  void *m_rawMem = nullptr;
  Slot *m_slots = nullptr;

  static constexpr size_t REAL_CAP = std::bit_ceil(Capacity);
  static constexpr size_t SLOT_MASK = REAL_CAP - 1;
  static constexpr size_t CAP_SHIFT = std::countr_zero(REAL_CAP);
  static constexpr size_t TOTAL_BYTES = REAL_CAP * sizeof(Slot);
  static constexpr size_t PAGE_SIZE = 4096;

  static constexpr size_t BATCH_PAGES = 16;
  static constexpr size_t BLOCK_SIZE = PAGE_SIZE * BATCH_PAGES;
  // Rounded down to a power of two, at least one slot.
  static constexpr size_t SLOTS_PER_BLOCK =
      std::min<size_t>(std::bit_floor(std::max<size_t>(
                           BLOCK_SIZE / sizeof(Slot), 1)),
                       REAL_CAP);
  static constexpr size_t NUM_BLOCKS = REAL_CAP / SLOTS_PER_BLOCK;

  static constexpr bool RECLAIM = NUM_BLOCKS > 1;
  static constexpr size_t GRANULE_SLOTS = RECLAIM ? SLOTS_PER_BLOCK : 1;
  static constexpr size_t GRANULE_SHIFT = std::countr_zero(GRANULE_SLOTS);
  static constexpr size_t NUM_GRANULES = REAL_CAP / GRANULE_SLOTS;

  alignas(64) std::atomic<size_t> m_head{0};
  alignas(64) std::atomic<size_t> m_tail{0};

  // Lap each granule currently serves.
  std::unique_ptr<std::atomic<size_t>[]> m_laps;
  // Slots popped per granule in its current lap, only used when reclaiming.
  std::unique_ptr<std::atomic<size_t>[]> m_pops;
};

} // namespace sc::threading
