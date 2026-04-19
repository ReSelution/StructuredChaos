//
// Created by oleub on 19.03.26.
//

#include "chaos_monotonic_resource.hpp"

#include "spdlog/async_logger.h"


namespace SC {
  void ChaosMonotonicResource::release() noexcept {
    std::lock_guard lock(m_block_mutex);
    for (const auto &block: m_blocks) {
      m_upstream->deallocate(block.ptr, block.size);
      CHAOS_MEM_RECORD(MemoryCapacity, -block.size)
    }
    CHAOS_MEM_RECORD(MemoryBlocks, -m_blocks.size())
#ifndef DISABLE_CHAOS_MEMORY_STATS
    uint64_t w = waste.load(std::memory_order_acquire);
    CHAOS_MEM_RECORD(MemoryWaste, -w)
    waste.fetch_sub(w);
#endif
    m_blocks.clear();
    if (m_initial_block.ptr) {
      m_range.store({m_initial_block.ptr, m_initial_block.ptr + m_initial_block.size}, std::memory_order_release);
    } else {
      m_range.store({nullptr, nullptr}, std::memory_order_release);
    }
  }

  void *ChaosMonotonicResource::do_allocate(size_t bytes, size_t alignment) {
    CHAOS_MEM_RECORD(AllocationCount, 1)
    while (true) {
      auto block = m_range.load(std::memory_order::acquire);
      if (!block.current) {
        handle_full_block(bytes, alignment);
        continue;
      }
      auto curr_addr = reinterpret_cast<uintptr_t>(block.current);
      uintptr_t aligned_addr = (curr_addr + alignment - 1) & ~(alignment - 1);
      auto aligned_ptr = reinterpret_cast<std::byte *>(aligned_addr);
      BlockRange next{.current =aligned_ptr + bytes, .end = block.end};

      if (next.current <= block.end) {
        if (m_range.compare_exchange_weak(block, next,
                                          std::memory_order_acq_rel)) {
          CHAOS_MEM_RECORD(MemoryRequested, bytes)
          CHAOS_MEM_RECORD(MemoryWaste, static_cast<uint64_t>(aligned_addr - curr_addr))
#ifndef DISABLE_CHAOS_MEMORY_STATS
          waste.fetch_add(static_cast<uint64_t>(aligned_addr - curr_addr), std::memory_order_relaxed);
#endif
          return aligned_ptr;
        }
        CHAOS_MEM_RECORD(ContentionCount, 1);
      } else {
        if (bytes < m_next_block_size) {
          handle_full_block(bytes, alignment);
        } else{

          return allocate_custom_block(bytes, alignment);
        }

      }
    }
  }

  void ChaosMonotonicResource::handle_full_block(size_t bytes, size_t alignment) {
    std::lock_guard lock(m_block_mutex);

    auto block = m_range.load(std::memory_order::relaxed);
    auto curr_addr = reinterpret_cast<uintptr_t>(block.current);
    uintptr_t aligned_addr = (curr_addr + alignment - 1) & ~(alignment - 1);

    if (block.current && (reinterpret_cast<std::byte *>(aligned_addr) + bytes <= block.end)) {
      return;
    }
    CHAOS_MEM_RECORD(MemoryWaste, block.end - block.current)
#ifndef DISABLE_CHAOS_MEMORY_STATS
    waste.fetch_add(block.end - block.current, std::memory_order_relaxed);
#endif
    allocate_new_block(m_next_block_size);
  }

  void ChaosMonotonicResource::allocate_new_block(size_t size) {
    void *ptr = m_upstream->allocate(size, alignof(std::max_align_t));
    CHAOS_MEM_RECORD(MemoryCapacity, size)
    CHAOS_MEM_RECORD(BlockOverflowCount, 1)
    CHAOS_MEM_RECORD(MemoryBlocks, 1)
    auto *b_ptr = static_cast<std::byte *>(ptr);
    m_blocks.emplace_back(b_ptr, size);
    m_range.store({b_ptr, b_ptr + size}, std::memory_order_release);
  }

  void *ChaosMonotonicResource::allocate_custom_block(size_t size, size_t alignment) {
    CHAOS_MEM_RECORD(MemoryCapacity, size)
    CHAOS_MEM_RECORD(BlockOverflowCount, 1)
    CHAOS_MEM_RECORD(MemoryBlocks, 1)
    CHAOS_MEM_RECORD(CustomBlocksCount, 1)
    CHAOS_MEM_RECORD(MemoryRequested, size)
    std::lock_guard lock(m_block_mutex);
    void *ptr = m_upstream->allocate(size, alignment);
    auto *b_ptr = static_cast<std::byte *>(ptr);
    m_blocks.emplace_back(b_ptr, size);
    return ptr;
  }
} // SC
