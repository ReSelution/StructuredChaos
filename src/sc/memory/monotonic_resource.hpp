#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <mutex>
#include <vector>

namespace sc {

class MonotonicResource : public std::pmr::memory_resource {
  struct Block {
    std::byte *ptr;
    size_t size;
    // Has to be handed back to the upstream exactly as it was requested.
    size_t alignment;
  };

  struct BlockRange {
    std::byte *current;
    std::byte *end;
  };

  alignas(64) std::atomic<BlockRange> m_range;
  alignas(64) std::mutex m_block_mutex;
  std::vector<Block> m_blocks;
  Block m_initial_block{};
  memory_resource *m_upstream;
  size_t m_next_block_size;

public:
  explicit MonotonicResource(
      size_t initial_size = 1024 * 32,
      memory_resource *upstream = std::pmr::get_default_resource())
      : m_upstream(upstream), m_next_block_size(initial_size) {
    allocate_new_block(initial_size);
  }

  explicit MonotonicResource(
      void *buffer, size_t size,
      memory_resource *upstream = std::pmr::get_default_resource())
      : m_initial_block(static_cast<std::byte *>(buffer), size,
                        alignof(std::max_align_t)),
        m_upstream(upstream), m_next_block_size(size * 2) {
    m_range.store({m_initial_block.ptr, m_initial_block.ptr + size},
                  std::memory_order_release);
  }

  MonotonicResource(const MonotonicResource &) = delete;

  ~MonotonicResource() override { release(); }

  void release() noexcept;

  [[nodiscard]] static bool checkLockFree() {
    return std::atomic<BlockRange>{}.is_lock_free();
  }

protected:
  void *do_allocate(size_t bytes, size_t alignment) override {
    while (true) {
      auto block = m_range.load(std::memory_order::acquire);
      if (std::byte *aligned_ptr = fit(block, bytes, alignment)) {
        BlockRange next{.current = aligned_ptr + bytes, .end = block.end};
        if (m_range.compare_exchange_weak(block, next,
                                          std::memory_order_acq_rel)) {

          return aligned_ptr;
        }
        continue;
      }

      if (!fits_new_block(bytes, alignment)) {

        return allocate_custom_block(bytes, alignment);
      }
      handle_full_block(bytes, alignment);
    }
  }

  void do_deallocate(void *, size_t, size_t) final {}

  [[nodiscard]] bool
  do_is_equal(const memory_resource &other) const noexcept override {
    return this == &other;
  }

private:
  // Start of the allocation inside the range, or nullptr if it does not fit.
  // Compares sizes instead of pointers, because current + bytes can wrap
  // around for huge requests.
  [[nodiscard]] static std::byte *fit(const BlockRange &range, size_t bytes,
                                      size_t alignment) noexcept {
    if (!range.current) {
      return nullptr;
    }
    auto curr_addr = reinterpret_cast<uintptr_t>(range.current);
    auto end_addr = reinterpret_cast<uintptr_t>(range.end);
    uintptr_t aligned_addr = (curr_addr + alignment - 1) & ~(alignment - 1);
    if (aligned_addr < curr_addr || aligned_addr > end_addr ||
        bytes > end_addr - aligned_addr) {
      return nullptr;
    }
    return range.current + (aligned_addr - curr_addr);
  }

  // New blocks are only aligned to max_align_t, so a stricter request can
  // lose up to alignment bytes of a fresh block to padding.
  [[nodiscard]] bool fits_new_block(size_t bytes,
                                    size_t alignment) const noexcept {
    if (bytes >= m_next_block_size) {
      return false;
    }
    return alignment <= alignof(std::max_align_t) ||
           alignment <= m_next_block_size - bytes;
  }

  // Gets a block from the upstream and records it for release().
  std::byte *allocate_block(size_t size, size_t alignment);
  void handle_full_block(size_t bytes, size_t alignment);
  void *allocate_custom_block(size_t size, size_t alignment);
  void allocate_new_block(size_t size);
};
} // namespace sc
