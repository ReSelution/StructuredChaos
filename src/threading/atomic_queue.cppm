module;

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

export module sc.threading:atomic_queue;

namespace sc {

  export template<typename T, size_t Capacity = 4096>
  class AtomicQueue {


    struct Slot {
      // 0 = EMPTY, 1 = STORED
      alignas(64) std::atomic<size_t> state{0};
      alignas(alignof(T)) char storage[sizeof(T)];
    };

  public:
    AtomicQueue() {
      size_t total_bytes = REAL_CAP * sizeof(Slot);
      m_release_counters = std::make_unique<std::atomic<uint16_t>[]>(NUM_BLOCKS);
#if defined(_WIN32)
      m_rawMem = VirtualAlloc(nullptr, total_bytes, MEM_RESERVE, PAGE_READWRITE);
#else
      m_rawMem = mmap(nullptr, total_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
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
      munmap(m_rawMem, REAL_CAP * sizeof(Slot));
#endif
    }

    AtomicQueue(const AtomicQueue &)            = delete;
    AtomicQueue &operator=(const AtomicQueue &) = delete;

    template<typename... Args>
    void emplace(Args &&...args) {
      size_t pos = m_tail.fetch_add(1, std::memory_order_relaxed);

      // Behebt die Race Condition: Stellt sicher, dass der Speicher da ist
      acquire(pos);

      Slot *slot = &m_slots[pos & SLOT_MASK];
      while (slot->state.load(std::memory_order_acquire) != 0) {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#endif
      }
      auto *item_ptr = reinterpret_cast<T *>(&slot->storage);
      ::new (static_cast<void *>(item_ptr)) T(std::forward<Args>(args)...);
      slot->state.store(1, std::memory_order_release);
    }

    void push(T &&t) { emplace(std::move(t)); }

    bool try_pop(T &t, int worker_id) {

      size_t pos = m_head.load(std::memory_order_relaxed);
      Slot *slot = nullptr;

      while (true) {
        slot         = &m_slots[pos & SLOT_MASK];
        size_t state = slot->state.load(std::memory_order_acquire);

        if (state == 0) [[unlikely]] {
          return false;
        }

        if (m_head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) [[likely]] {
          break;
        }


        const int spins = 2 + (worker_id & 3); // Ergibt 2, 3, 4 oder 5 Spins
        for (int i = 0; i < spins; ++i) {
#if defined(__x86_64__) || defined(_M_X64)
          _mm_pause();
#elif defined(__ARM_ARCH) || defined(__aarch64__)
          asm volatile("yield" ::: "memory");
#endif
        }
      }

      auto *item_ptr = reinterpret_cast<T *>(&slot->storage);
      t              = std::move(*item_ptr);
      item_ptr->~T();

      slot->state.store(0, std::memory_order_release);
      release(pos);
      return true;
    }


    size_t size() const {
      const auto h    = m_head.load(std::memory_order_relaxed);
      const auto t    = m_tail.load(std::memory_order_relaxed);
      const auto diff = static_cast<ptrdiff_t>(t - h);
      return static_cast<size_t>(std::max<ptrdiff_t>(0, diff));
    }

    bool empty() const { return !size(); }

    static size_t num_blocks() { return NUM_BLOCKS; }

    static size_t slots_per_block() { return SLOTS_PER_BLOCK; }

  private:
    void acquire(size_t global_idx) {

      if constexpr (SLOTS_PER_BLOCK == 0)
        return;

      size_t block_id = get_block_id(global_idx);

      if ((global_idx & BLOCK_SLOT_MASK) == 0) {
        m_release_counters[block_id].fetch_add(SLOTS_PER_BLOCK, std::memory_order_relaxed);
      }

#if defined(_WIN32)
      // Jedes Ticket innerhalb einer Page stellt das Commit sicher (idempotent)
      if ((global_idx & (PAGE_SIZE / sizeof(Slot) - 1)) == 0) {
        size_t page_start_slot = global_idx & SLOT_MASK;
        VirtualAlloc(static_cast<void *>(m_slots + page_start_slot), PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE);
      }
#endif
    }

    void release(size_t global_idx) {
      size_t block_id = get_block_id(global_idx);

      // Fällt der Zähler auf 0 (fetch_sub liefert 1), ist der Block physisch leer verarbeitet.
      if (m_release_counters[block_id].fetch_sub(1, std::memory_order_acq_rel) == 1) {
        size_t target_start_slot  = (global_idx & ~BLOCK_SLOT_MASK) & SLOT_MASK;
        void *const block_address = static_cast<void *>(m_slots + target_start_slot);

#if defined(_WIN32)
        VirtualFree(block_address, BLOCK_SIZE, MEM_DECOMMIT);
#else
        // Direktes madvise ohne vorherigen m_head-Check, um False Sharing zu minimieren
        madvise(block_address, BLOCK_SIZE, MADV_FREE);
#endif
      }
    }

    [[nodiscard]] size_t get_block_id(size_t global_index) const noexcept {
      if constexpr (SLOTS_PER_BLOCK > 0) {
        return (global_index >> BLOCK_SHIFT) & BLOCK_MASK;
      }
      return 0;
    }

    void *m_rawMem = nullptr;
    Slot *m_slots  = nullptr;

    static constexpr size_t REAL_CAP  = std::bit_ceil(Capacity);
    static constexpr size_t SLOT_MASK = REAL_CAP - 1;
    static constexpr size_t PAGE_SIZE = 4096;

    // Stellschraube: Erhöhe BATCH_PAGES auf z.B. 64 oder 128, um die OS-Kosten zu strecken!
    static constexpr size_t BATCH_PAGES     = 16;
    static constexpr size_t BLOCK_SIZE      = PAGE_SIZE * BATCH_PAGES;
    static constexpr size_t SLOTS_PER_BLOCK = BLOCK_SIZE / sizeof(Slot);

    static constexpr size_t BLOCK_SHIFT     = std::countr_zero(SLOTS_PER_BLOCK);
    static constexpr size_t BLOCK_SLOT_MASK = SLOTS_PER_BLOCK - 1;

    static constexpr size_t NUM_BLOCKS = REAL_CAP / (SLOTS_PER_BLOCK > 0 ? SLOTS_PER_BLOCK : 1);
    static constexpr size_t BLOCK_MASK = NUM_BLOCKS - 1;

    alignas(64) std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};

    std::unique_ptr<std::atomic<uint16_t>[]> m_release_counters;
  };

} // namespace sc
