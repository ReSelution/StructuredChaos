module;

#include <atomic>
#include <bit>
#include <cstddef>
#include <utility>

#if defined(_WIN32)
#include <intrin.h> // Für _mm_pause
#include <windows.h>
#else
#include <sys/mman.h>
#include <x86intrin.h> // Für _mm_pause auf x86/x64
#endif

export module SC.Threading:AtomicQueue;

namespace SC {

  export template<typename T, size_t Capacity = 4096>
  class AtomicQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity muss eine Zweierpotenz sein!");

    struct Slot {
      // 0 = EMPTY, 1 = STORED
      alignas(64) std::atomic<size_t> state{0};
      alignas(alignof(T)) char storage[sizeof(T)];
    };

  public:
    AtomicQueue() {
      size_t total_bytes = Capacity * sizeof(Slot);

#if defined(_WIN32)
      m_rawMem = VirtualAlloc(nullptr, total_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
      m_rawMem = mmap(nullptr, total_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
      m_slots = reinterpret_cast<Slot *>(m_rawMem);
    }

    ~AtomicQueue() noexcept {
      size_t h = m_head.load(std::memory_order_relaxed);
      size_t t = m_tail.load(std::memory_order_relaxed);
      while (h < t) {
        auto *slot_ptr = reinterpret_cast<T *>(&m_slots[h & SLOT_MASK].storage);
        slot_ptr->~T();
        h++;
      }

#if defined(_WIN32)
      VirtualFree(m_rawMem, 0, MEM_RELEASE);
#else
      munmap(m_rawMem, Capacity * sizeof(Slot));
#endif
    }

    AtomicQueue(const AtomicQueue &)            = delete;
    AtomicQueue &operator=(const AtomicQueue &) = delete;

    template<typename... Args>
    void emplace(Args &&...args) {
      size_t pos = m_tail.fetch_add(1, std::memory_order_relaxed);
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

    bool try_pop(T &t) {
      size_t pos = m_head.load(std::memory_order_relaxed);

      while (true) {
        // 1. Schau direkt auf den Slot, den wir uns schnappen wollen
        Slot *slot   = &m_slots[pos & SLOT_MASK];
        size_t state = slot->state.load(std::memory_order_acquire);

        // Wenn der Slot 0 (EMPTY) ist, gibt es zwei Möglichkeiten:
        // Entweder die Queue ist wirklich leer, oder ein Producer hat zwar
        // m_tail erhöht, aber die Daten noch nicht reingeschrieben.
        // In BEIDEN Fällen müssen wir sofort abbrechen (try_pop darf nicht blockieren!).
        if (state == 0) {
          return false;
        }

        // 2. Nur wenn der Slot wirklich den Status 1 (STORED) hat,
        // versuchen wir, das Ticket m_head per CAS zu inkrementieren.
        if (m_head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          break; // Ticket erfolgreich gesichert!
        }

        // CAS fehlgeschlagen (ein anderer Consumer war schneller).
        // 'pos' wurde automatisch aktualisiert, wir atmen kurz durch und prüfen den nächsten Slot.
#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#endif
      }

      // HIER angekommen, besitzen WIR das Ticket 'pos' exklusiv.
      // Da wir oben im 'state == 1' Check waren, sind die Daten garantiert bereit.
      Slot *slot = &m_slots[pos & SLOT_MASK];

      auto *item_ptr = reinterpret_cast<T *>(&slot->storage);
      t              = std::move(*item_ptr);
      item_ptr->~T();

      // Slot wieder freigeben
      slot->state.store(0, std::memory_order_release);

      // Virtuellen Speicher aufräumen
      release(pos);
      return true;
    }

  private:
    void acquire(size_t global_idx) {
      if constexpr (SLOTS_PER_BLOCK == 0)
        return;

      // NUR der Thread, der den ALLERERSTEN Slot eines Blocks erwischt,
      // erhöht den Counter im Voraus um die volle Blockgröße!
      if ((global_idx & BLOCK_SLOT_MASK) == 0) {
        size_t block_id = get_block_id(global_idx);
        m_release_counters[block_id].fetch_add(SLOTS_PER_BLOCK, std::memory_order_relaxed);

#if defined(_WIN32)
        size_t target_start_slot = (global_idx & ~BLOCK_SLOT_MASK) & SLOT_MASK;
        VirtualAlloc(static_cast<void *>(m_slots + target_start_slot), BLOCK_SIZE, MEM_COMMIT, PAGE_READWRITE);
#endif
      }
    }

    void release(size_t global_idx) {
      size_t block_id = get_block_id(global_idx);

      if (m_release_counters[block_id].fetch_sub(1, std::memory_order_acq_rel) == 1) {
        size_t current_head  = m_head.load(std::memory_order_relaxed);
        size_t block_end_idx = (global_idx & ~BLOCK_SLOT_MASK) + BLOCK_SLOT_MASK;

        if (current_head > block_end_idx) {
          size_t target_start_slot  = (global_idx & ~BLOCK_SLOT_MASK) & SLOT_MASK;
          void *const block_address = static_cast<void *>(m_slots + target_start_slot);

#if defined(_WIN32)
          VirtualFree(block_address, BLOCK_SIZE, MEM_DECOMMIT);
#else
          madvise(block_address, BLOCK_SIZE, MADV_DONTNEED);
#endif
        }
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

    static constexpr size_t SLOT_MASK       = Capacity - 1;
    static constexpr size_t PAGE_SIZE       = 4096;
    static constexpr size_t BATCH_PAGES     = 16;
    static constexpr size_t BLOCK_SIZE      = PAGE_SIZE * BATCH_PAGES;
    static constexpr size_t SLOTS_PER_BLOCK = BLOCK_SIZE / sizeof(Slot);

    static constexpr size_t BLOCK_SHIFT     = std::countr_zero(SLOTS_PER_BLOCK);
    static constexpr size_t BLOCK_SLOT_MASK = SLOTS_PER_BLOCK - 1;

    static constexpr size_t NUM_BLOCKS = Capacity / (SLOTS_PER_BLOCK > 0 ? SLOTS_PER_BLOCK : 1);
    static constexpr size_t BLOCK_MASK = NUM_BLOCKS - 1;

    alignas(64) std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};
    alignas(64) std::atomic<size_t> m_release_counters[NUM_BLOCKS]{};
  };

} // namespace SC
