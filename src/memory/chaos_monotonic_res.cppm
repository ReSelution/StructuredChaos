module;

// Alle externen Standard-Header gehören in die globale Modul-Präambel

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <mutex>
#include <vector>

export module SC.Memory:MRes;
import SC.Stats;

namespace SC {

#define DISABLE_CHAOS_MEMORY_STATS

// ============================================================================
// Neues C++20 Typ-basiertes Stat-System
// ============================================================================
export using StatCapacity =
    SC::ChaosStat<"MemoryCapacity", SC::ChaosThroughput<>>;
export using StatBlocks =
    SC::ChaosStat<"MemoryBlocks", SC::ChaosThroughput<SC::MetricUnits>>;
export using StatRequested =
    SC::ChaosStat<"MemoryRequested", SC::ChaosThroughput<>>;
export using StatWaste = SC::ChaosStat<"MemoryWaste", SC::ChaosThroughput<>>;
export using StatAllocCount =
    SC::ChaosStat<"AllocationCount", SC::ChaosThroughput<SC::MetricUnits>>;
export using StatContention =
    SC::ChaosStat<"ContentionCount", SC::ChaosThroughput<SC::MetricUnits>>;
export using StatOverflow =
    SC::ChaosStat<"BlockOverflowCount", SC::ChaosThroughput<SC::MetricUnits>>;
export using StatCustomBlocks =
    SC::ChaosStat<"CustomBlocks", SC::ChaosThroughput<SC::MetricUnits>>;

#ifdef DISABLE_CHAOS_MEMORY_STATS
#define CHAOS_MEM_RECORD(StatType, val) ((void)0);
#else
#define CHAOS_MEM_RECORD(StatType, val) StatType::record(val);
#endif

export class ChaosMonotonicResource : public std::pmr::memory_resource {
  struct Block {
    std::byte *ptr;
    size_t size;
  };

  struct BlockRange {
    std::byte *current;
    std::byte *end;
  };

  alignas(64) std::atomic<BlockRange> m_range;
  alignas(64) std::mutex m_block_mutex;
  std::vector<Block> m_blocks;
  Block m_initial_block{nullptr, 0};
  std::pmr::memory_resource *m_upstream;
  size_t m_next_block_size;

#ifndef DISABLE_CHAOS_MEMORY_STATS
  std::atomic<uint64_t> m_waste_accumulator{0};
#endif

public:
  explicit ChaosMonotonicResource(
      size_t initial_size = 1024 * 32,
      std::pmr::memory_resource *upstream = std::pmr::get_default_resource())
      : m_upstream(upstream), m_next_block_size(initial_size) {
    allocate_new_block(initial_size);
  }

  explicit ChaosMonotonicResource(
      void *buffer, size_t size,
      std::pmr::memory_resource *upstream = std::pmr::get_default_resource())
      : m_upstream(upstream),
        m_initial_block(static_cast<std::byte *>(buffer), size),
        m_next_block_size(size * 2) {
    m_range.store({m_initial_block.ptr, m_initial_block.ptr + size},
                  std::memory_order_release);
    CHAOS_MEM_RECORD(StatCapacity, size)
  }

  ChaosMonotonicResource(const ChaosMonotonicResource &) = delete;
  ChaosMonotonicResource &operator=(const ChaosMonotonicResource &) = delete;

  ~ChaosMonotonicResource() override { release(); }

  void release() noexcept {
    std::lock_guard lock(m_block_mutex);

    size_t total_capacity = 0;
    for (const auto &block : m_blocks) {
      m_upstream->deallocate(block.ptr, block.size);
      total_capacity += block.size;
    }

    CHAOS_MEM_RECORD(StatCapacity, -static_cast<int64_t>(total_capacity))
    if (m_initial_block.ptr) {
      CHAOS_MEM_RECORD(StatCapacity,
                       -static_cast<int64_t>(m_initial_block.size))
    }

    CHAOS_MEM_RECORD(StatBlocks, -static_cast<int64_t>(m_blocks.size()))

#ifndef DISABLE_CHAOS_MEMORY_STATS
    uint64_t w = m_waste_accumulator.load(std::memory_order::acquire);
    CHAOS_MEM_RECORD(StatWaste, -static_cast<int64_t>(w))
    m_waste_accumulator.store(0, std::memory_order_relaxed);
#endif

    m_blocks.clear();

    if (m_initial_block.ptr) {
      m_range.store(
          {m_initial_block.ptr, m_initial_block.ptr + m_initial_block.size},
          std::memory_order_release);
    } else {
      m_range.store({nullptr, nullptr}, std::memory_order_release);
    }
  }

  [[nodiscard]] bool checkLockFree() const { return m_range.is_lock_free(); }

protected:
  void *do_allocate(size_t bytes, size_t alignment) final {
    CHAOS_MEM_RECORD(StatAllocCount, 1)

    while (true) {
      auto block = m_range.load(std::memory_order::acquire);
      if (!block.current) {
        handle_full_block(bytes, alignment);
        continue;
      }

      auto curr_addr = reinterpret_cast<uintptr_t>(block.current);
      uintptr_t aligned_addr = (curr_addr + alignment - 1) & ~(alignment - 1);
      auto aligned_ptr = reinterpret_cast<std::byte *>(aligned_addr);
      BlockRange next{.current = aligned_ptr + bytes, .end = block.end};

      if (next.current <= block.end) {
        if (m_range.compare_exchange_weak(block, next,
                                          std::memory_order_acq_rel)) {
          uint64_t alignment_waste =
              static_cast<uint64_t>(aligned_addr - curr_addr);

          CHAOS_MEM_RECORD(StatRequested, bytes)
          CHAOS_MEM_RECORD(StatWaste, alignment_waste)

#ifndef DISABLE_CHAOS_MEMORY_STATS
          m_waste_accumulator.fetch_add(alignment_waste,
                                        std::memory_order_relaxed);
#endif
          return aligned_ptr;
        }
        CHAOS_MEM_RECORD(StatContention, 1);
      } else {
        if (bytes < m_next_block_size) {
          handle_full_block(bytes, alignment);
        } else {
          return allocate_custom_block(bytes, alignment);
        }
      }
    }
  }

  void do_deallocate(void *, size_t, size_t) final {}

  [[nodiscard]] bool
  do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
    return this == &other;
  }

private:
  void handle_full_block(size_t bytes, size_t alignment) {
    std::lock_guard lock(m_block_mutex);

    auto block = m_range.load(std::memory_order::relaxed);
    if (block.current) {
      auto curr_addr = reinterpret_cast<uintptr_t>(block.current);
      uintptr_t aligned_addr = (curr_addr + alignment - 1) & ~(alignment - 1);

      if (reinterpret_cast<std::byte *>(aligned_addr) + bytes <= block.end) {
        return;
      }

      uint64_t remaining_waste = block.end - block.current;
      CHAOS_MEM_RECORD(StatWaste, remaining_waste)
#ifndef DISABLE_CHAOS_MEMORY_STATS
      m_waste_accumulator.fetch_add(remaining_waste, std::memory_order_relaxed);
#endif
    }

    allocate_new_block(m_next_block_size);
    m_next_block_size *= 2;
  }

  void *allocate_custom_block(size_t size, size_t alignment) {
    CHAOS_MEM_RECORD(StatCapacity, size)
    CHAOS_MEM_RECORD(StatOverflow, 1)
    CHAOS_MEM_RECORD(StatBlocks, 1)
    CHAOS_MEM_RECORD(StatCustomBlocks, 1)
    CHAOS_MEM_RECORD(StatRequested, size)

    std::lock_guard lock(m_block_mutex);
    void *ptr = m_upstream->allocate(size, alignment);
    auto *b_ptr = static_cast<std::byte *>(ptr);

    m_blocks.emplace_back(b_ptr, size);
    return ptr;
  }

  void allocate_new_block(size_t size) {
    void *ptr = m_upstream->allocate(size, alignof(std::max_align_t));

    CHAOS_MEM_RECORD(StatCapacity, size)
    CHAOS_MEM_RECORD(StatOverflow, 1)
    CHAOS_MEM_RECORD(StatBlocks, 1)

    auto *b_ptr = static_cast<std::byte *>(ptr);
    m_blocks.emplace_back(b_ptr, size);
    m_range.store({b_ptr, b_ptr + size}, std::memory_order_release);
  }
};

} // namespace SC
