#pragma once

#include <memory_resource>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

#include "stats/chaos_stats.hpp"
#include "stats/chaos_throughput.hpp"

namespace SC {
#define  DISABLE_CHAOS_MEMORY_STATS

#ifdef DISABLE_CHAOS_MEMORY_STATS
  // Wenn deaktiviert: Macros tun nichts oder erzeugen minimale Dummies
    #define DEFINE_MEMORY_STAT(name, label, type, hidden);
    #define REGISTER_MEMORY_STAT(name);
    #define CHAOS_MEM_RECORD(name, val) ((void)0);
#else
  // Wenn aktiviert: Mapping auf deine echten Chaos-Stats
#define DEFINE_MEMORY_STAT(name, label, type, hidden) \
        DEFINE_CHAOS_STAT(name, label, type, hidden)
#define REGISTER_MEMORY_STAT(name) \
        REGISTER_CHAOS_STAT(name)
#define CHAOS_MEM_RECORD(name, val) CHAOS_MEM_RECORD(name, val)
#endif


  DEFINE_MEMORY_STAT(MemoryCapacity, "MemoryCapacity", SC::ChaosThroughput<>, false);
  REGISTER_MEMORY_STAT(MemoryCapacity)

  DEFINE_MEMORY_STAT(MemoryBlocks, "MemoryBlocks", SC::ChaosThroughput<SC::MetricUnits>, false);
  REGISTER_MEMORY_STAT(MemoryBlocks)

  DEFINE_MEMORY_STAT(MemoryRequested, "MemoryRequested", SC::ChaosThroughput<>, false);
  REGISTER_MEMORY_STAT(MemoryRequested)

  DEFINE_MEMORY_STAT(MemoryWaste, "MemoryWaste", SC::ChaosThroughput<>, false);
  REGISTER_MEMORY_STAT(MemoryWaste)

  DEFINE_MEMORY_STAT(AllocationCount, "AllocationCount", SC::ChaosThroughput<SC::MetricUnits>, false);
  REGISTER_MEMORY_STAT(AllocationCount)

  DEFINE_MEMORY_STAT(ContentionCount, "ContentionCount", SC::ChaosThroughput<SC::MetricUnits>, false);
  REGISTER_MEMORY_STAT(ContentionCount)

  DEFINE_MEMORY_STAT(BlockOverflowCount, "BlockOverflowCount", SC::ChaosThroughput<SC::MetricUnits>, false);
  REGISTER_MEMORY_STAT(BlockOverflowCount)

  DEFINE_MEMORY_STAT(CustomBlocksCount, "CustomBlocks", SC::ChaosThroughput<SC::MetricUnits>, false);
  REGISTER_MEMORY_STAT(CustomBlocksCount)


  class ChaosMonotonicResource : public std::pmr::memory_resource {
    struct Block {
      std::byte *ptr;
      size_t size;
    };

    struct BlockRange {
      std::byte *current;
      std::byte *end;
    };


    alignas(64) std::atomic<BlockRange> m_range;
    std::atomic<size_t> usedBytes;
    alignas(64) std::mutex m_block_mutex;
    std::vector<Block> m_blocks;
    Block m_initial_block;
    memory_resource *m_upstream;
    size_t m_next_block_size;
#ifndef DISABLE_CHAOS_MEMORY_STATS
    std::atomic<uint64_t> waste {};
#endif

  public:
    explicit ChaosMonotonicResource(size_t initial_size = 1024 * 32,
                                    memory_resource *upstream = std::pmr::get_default_resource())
            : m_upstream(upstream), m_next_block_size(initial_size) {
      allocate_new_block(initial_size);
    }

    explicit ChaosMonotonicResource(void *buffer, size_t size,
                                    memory_resource *upstream = std::pmr::get_default_resource())
            : m_upstream(upstream), m_initial_block(static_cast<std::byte *>(buffer), size),
              m_next_block_size(size * 2) {
      m_range.store({m_initial_block.ptr, m_initial_block.ptr + size}, std::memory_order_release);
      CHAOS_MEM_RECORD(MemoryCapacity, size)
    }

    ChaosMonotonicResource(const ChaosMonotonicResource &) = delete;

    ~ChaosMonotonicResource() override {
      if (m_initial_block.ptr) {
        CHAOS_MEM_RECORD(MemoryCapacity, m_initial_block.size)
      }
      release();
    }

    void release() noexcept;

    [[nodiscard]] bool checkLockFree() const {
      return m_range.is_lock_free();
    }

  protected:
    void *do_allocate(size_t bytes, size_t alignment) final;

    void do_deallocate(void *, size_t, size_t) final {
    }

    [[nodiscard]] bool do_is_equal(const memory_resource &other) const noexcept override {
      return this == &other;
    }

  private:
    void handle_full_block(size_t bytes, size_t alignment);
    void *allocate_custom_block(size_t size, size_t alignment);
    void allocate_new_block(size_t size);
  };
} // SC
