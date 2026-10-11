//
// Created by oleub on 19.03.26.
//

#include "monotonic_resource.hpp"

namespace sc {
  void MonotonicResource::reset() noexcept {
    runCleanups();
    std::lock_guard lock(m_block_mutex);
    // With a caller buffer every block is an extra one, otherwise the first
    // block is the one to start over in.
    const size_t kept = (m_initial_block.ptr == nullptr && !m_blocks.empty()) ? 1 : 0;
    for (size_t i = kept; i < m_blocks.size(); ++i) {
      m_upstream->deallocate(m_blocks[i].ptr, m_blocks[i].size, m_blocks[i].alignment);
    }
    m_blocks.resize(kept);

    if (m_initial_block.ptr) {
      m_range.store({m_initial_block.ptr, m_initial_block.ptr + m_initial_block.size}, std::memory_order_release);
    } else if (kept != 0) {
      m_range.store({m_blocks[0].ptr, m_blocks[0].ptr + m_blocks[0].size}, std::memory_order_release);
    } else {
      m_range.store({nullptr, nullptr}, std::memory_order_release);
    }
  }

  void MonotonicResource::release() noexcept {
    runCleanups();
    std::lock_guard lock(m_block_mutex);
    for (const auto &block : m_blocks) {
      m_upstream->deallocate(block.ptr, block.size, block.alignment);
    }

    m_blocks.clear();
    if (m_initial_block.ptr) {
      m_range.store({m_initial_block.ptr, m_initial_block.ptr + m_initial_block.size}, std::memory_order_release);
    } else {
      m_range.store({nullptr, nullptr}, std::memory_order_release);
    }
  }

  void MonotonicResource::handle_full_block(size_t bytes, size_t alignment) {
    std::lock_guard lock(m_block_mutex);

    // Another thread may have installed a new block in the meantime.
    if (fit(m_range.load(std::memory_order::relaxed), bytes, alignment)) {
      return;
    }

    allocate_new_block(m_next_block_size);
  }

  std::byte *MonotonicResource::allocate_block(size_t size, size_t alignment) {
    void *ptr = m_upstream->allocate(size, alignment);
    auto *b_ptr = static_cast<std::byte *>(ptr);
    try {
      m_blocks.emplace_back(b_ptr, size, alignment);
    } catch (...) {
      m_upstream->deallocate(ptr, size, alignment);
      throw;
    }
    return b_ptr;
  }

  void MonotonicResource::allocate_new_block(size_t size) {
    auto *b_ptr = allocate_block(size, alignof(std::max_align_t));
    m_range.store({b_ptr, b_ptr + size}, std::memory_order_release);
  }

  void *MonotonicResource::allocate_custom_block(size_t size, size_t alignment) {

    std::lock_guard lock(m_block_mutex);
    return allocate_block(size, alignment);
  }
} // namespace sc
