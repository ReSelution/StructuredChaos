#pragma once

#include <memory_resource>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

#include "stats/chaos_stats.hpp"
#include "stats/chaos_throughput.hpp"

namespace SC {
  DEFINE_CHAOS_STAT(MemoryCapacity, "MemoryCapacity", SC::ChaosThroughput<>, false);
  REGISTER_CHAOS_STAT(MemoryCapacity)

  DEFINE_CHAOS_STAT(MemoryBlocks, "MemoryBlocks", SC::ChaosThroughput<SC::MetricUnits>, false);
  REGISTER_CHAOS_STAT(MemoryBlocks)

  DEFINE_CHAOS_STAT(MemoryRequested, "MemoryRequested", SC::ChaosThroughput<>, false);
  REGISTER_CHAOS_STAT(MemoryRequested)

  DEFINE_CHAOS_STAT(MemoryWaste, "MemoryWaste", SC::ChaosThroughput<>, false);
  REGISTER_CHAOS_STAT(MemoryWaste)

  DEFINE_CHAOS_STAT(AllocationCount, "AllocationCount", SC::ChaosThroughput<SC::MetricUnits>, false);
  REGISTER_CHAOS_STAT(AllocationCount)

  DEFINE_CHAOS_STAT(ContentionCount, "ContentionCount", SC::ChaosThroughput<SC::MetricUnits>, false);
  REGISTER_CHAOS_STAT(ContentionCount)

  DEFINE_CHAOS_STAT(BlockOverflowCount, "BlockOverflowCount", SC::ChaosThroughput<SC::MetricUnits>, false);
  REGISTER_CHAOS_STAT(BlockOverflowCount)

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
    std::atomic<uint64_t> waste {};

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
      CHAOS_RECORD(MemoryCapacity, size)
    }

    ChaosMonotonicResource(const ChaosMonotonicResource &) = delete;

    ~ChaosMonotonicResource() override {
      if (m_initial_block.ptr) {
        CHAOS_RECORD(MemoryCapacity, m_initial_block.size)
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

    void allocate_new_block(size_t size);
  };
} // SC
