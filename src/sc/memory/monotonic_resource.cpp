#include "monotonic_resource.hpp"

namespace sc {
  void MonotonicResource::reset() noexcept {
    m_cleanup.runAll();
    std::scoped_lock lock(m_blockMutex);
    // With a caller buffer every block is an extra one, otherwise the first
    // block is the one to start over in.
    const size_t kept = (m_initialBlock.ptr == nullptr && !m_blocks.empty()) ? 1 : 0;
    for (size_t i = kept; i < m_blocks.size(); ++i) {
      m_upstream->deallocate(m_blocks[i].ptr, m_blocks[i].size, m_blocks[i].alignment);
    }
    m_blocks.resize(kept);

    if (m_initialBlock.ptr != nullptr) {
      installRange(m_initialBlock.ptr, m_initialBlock.size);
    } else if (kept != 0) {
      installRange(m_blocks[0].ptr, m_blocks[0].size);
    } else {
      installRange(nullptr, 0);
    }
  }

  void MonotonicResource::release() noexcept {
    m_cleanup.runAll();
    std::scoped_lock lock(m_blockMutex);
    for (const auto &block: m_blocks) {
      m_upstream->deallocate(block.ptr, block.size, block.alignment);
    }

    m_blocks.clear();
    if (m_initialBlock.ptr != nullptr) {
      installRange(m_initialBlock.ptr, m_initialBlock.size);
    } else {
      installRange(nullptr, 0);
    }
  }

  void MonotonicResource::handleFullBlock(size_t bytes, size_t alignment) {
    std::scoped_lock lock(m_blockMutex);

    // Another thread may have installed a new block in the meantime.
    if (fit(m_current.load(std::memory_order::relaxed), m_end.load(std::memory_order::relaxed), bytes, alignment) !=
        nullptr) {
      return;
    }

    allocateNewBlock(m_blockSize);
  }

  std::byte *MonotonicResource::allocateBlock(size_t size, size_t alignment) {
    void *ptr = m_upstream->allocate(size, alignment);
    auto *block = static_cast<std::byte *>(ptr);
    try {
      m_blocks.emplace_back(block, size, alignment);
    } catch (...) {
      m_upstream->deallocate(ptr, size, alignment);
      throw;
    }
    return block;
  }

  void MonotonicResource::allocateNewBlock(size_t size) {
    auto *block = allocateBlock(size, alignof(std::max_align_t));
    installRange(block, size);
  }

  void *MonotonicResource::allocateCustomBlock(size_t size, size_t alignment) {
    std::scoped_lock lock(m_blockMutex);
    return allocateBlock(size, alignment);
  }
} // namespace sc
