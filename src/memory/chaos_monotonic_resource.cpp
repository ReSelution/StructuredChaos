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
//      CHAOS_RECORD(MemoryCapacity, -block.size)
    }
//    CHAOS_RECORD(MemoryBlocks, -m_blocks.size())
//    uint64_t w = waste.load(std::memory_order_acquire);
//    CHAOS_RECORD(MemoryWaste, -w)
//    waste.fetch_sub(w);
    m_blocks.clear();
    if (m_initial_block.ptr) {
      m_range.store({m_initial_block.ptr, m_initial_block.ptr + m_initial_block.size}, std::memory_order_release);
    } else {
      m_range.store({nullptr, nullptr}, std::memory_order_release);
    }
  }

  void *ChaosMonotonicResource::do_allocate(size_t bytes, size_t alignment) {
//    CHAOS_RECORD(AllocationCount, 1)
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
//          CHAOS_RECORD(MemoryRequested, bytes)
//          CHAOS_RECORD(MemoryWaste, static_cast<uint64_t>(aligned_addr - curr_addr))
//          waste.fetch_add( static_cast<uint64_t>(aligned_addr - curr_addr), std::memory_order_relaxed);
          return aligned_ptr;
        }
//        CHAOS_RECORD(ContentionCount, 1);
      } else {
        handle_full_block(bytes, alignment);
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
//    CHAOS_RECORD(MemoryWaste, block.end - block.current)
//    waste.fetch_add(block.end - block.current, std::memory_order_relaxed);
    size_t size_to_alloc = std::max(m_next_block_size, bytes + alignment);
    allocate_new_block(size_to_alloc);
  }

  void ChaosMonotonicResource::allocate_new_block(size_t size) {
    void *ptr = m_upstream->allocate(size, alignof(std::max_align_t));
//    CHAOS_RECORD(MemoryCapacity, size)
//    CHAOS_RECORD(BlockOverflowCount, 1)
//    CHAOS_RECORD(MemoryBlocks, 1)
    auto *b_ptr = static_cast<std::byte *>(ptr);
    m_blocks.emplace_back(b_ptr, size);
    m_range.store({b_ptr, b_ptr + size}, std::memory_order_release);
  }
} // SC
