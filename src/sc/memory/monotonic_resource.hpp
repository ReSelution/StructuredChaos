#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "sc/memory/pmr_traits.hpp"

namespace sc {

  // Bump allocator: hands out memory from blocks of its upstream and never
  // takes a single allocation back. reset() and release() free everything at
  // once.
  //
  // Objects created with make() or makePmr() are destroyed then, the newest
  // first. Nothing else is: what lies in a block from allocate() or
  // allocateSpan() gets no destructor call. Neither does a pmr container from
  // makePmr() that keeps everything in this resource, since its memory goes
  // with the resource anyway.
  //
  // Allocating and make() are safe from several threads at the same time.
  // reset(), release() and the destructor must not run while another thread
  // still does either.
  class MonotonicResource final : public std::pmr::memory_resource {
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

    // Remembers one object that needs its destructor run. Lies right behind
    // the object, in the same allocation.
    struct CleanupNode {
      void (*destroy)(CleanupNode *) noexcept;
      CleanupNode *next;
    };

    // Distance from the object to its node.
    template<typename T>
    static constexpr size_t nodeOffset = (sizeof(T) + alignof(CleanupNode) - 1) & ~(alignof(CleanupNode) - 1);

    alignas(64) std::atomic<BlockRange> m_range;
    alignas(64) std::mutex m_block_mutex;
    std::vector<Block> m_blocks;
    Block m_initial_block{};
    memory_resource *m_upstream;
    size_t m_next_block_size;
    std::atomic<CleanupNode *> m_cleanup{nullptr};

  public:
    explicit MonotonicResource(size_t initial_size = 1024 * 32,
                               memory_resource *upstream = std::pmr::get_default_resource()) :
        m_upstream(upstream), m_next_block_size(initial_size) {
      allocate_new_block(initial_size);
    }

    explicit MonotonicResource(void *buffer, size_t size,
                               memory_resource *upstream = std::pmr::get_default_resource()) :
        m_initial_block(static_cast<std::byte *>(buffer), size, alignof(std::max_align_t)), m_upstream(upstream),
        m_next_block_size(size * 2) {
      m_range.store({m_initial_block.ptr, m_initial_block.ptr + size}, std::memory_order_release);
    }

    MonotonicResource(const MonotonicResource &) = delete;

    MonotonicResource &operator=(const MonotonicResource &) = delete;

    ~MonotonicResource() override { release(); }

    // Hides the function of the same name in memory_resource, which would do
    // the same through a virtual call.
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    [[nodiscard]] void *allocate(size_t bytes, size_t alignment = alignof(std::max_align_t)) {
      while (true) {
        auto block = m_range.load(std::memory_order::acquire);
        if (std::byte *aligned_ptr = fit(block, bytes, alignment)) {
          BlockRange next{.current = aligned_ptr + bytes, .end = block.end};
          if (m_range.compare_exchange_weak(block, next, std::memory_order_acq_rel)) {

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

    // The object lives until the resource is reset, released or destroyed.
    // Its destructor is run then, if it has one. For a pmr container that is
    // to live in this resource, use makePmr().
    template<typename T, typename... Args>
    T *make(Args &&...args) {
      if constexpr (std::is_trivially_destructible_v<T>) {
        return new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
      } else {
        void *block = allocateWithNode<T>();
        // If the constructor throws, the block stays unused until the next
        // reset and nothing is remembered.
        T *object = new (block) T(std::forward<Args>(args)...);
        remember<T>(block);
        return object;
      }
    }

    // Like make(), for a type that takes a pmr allocator: the resource passes
    // itself as that allocator, behind or in front of args as the type wants
    // it. The object and everything it allocates lives in the resource, so a
    // container whose elements own nothing else needs no destructor call.
    template<typename T, typename... Args>
      requires std::uses_allocator_v<T, std::pmr::polymorphic_allocator<>>
    T *makePmr(Args &&...args) {
      const std::pmr::polymorphic_allocator<> allocator(this);
      if constexpr (detail::ownsOnlyResourceMemory<T>) {
        void *block = allocate(sizeof(T), alignof(T));
        return std::uninitialized_construct_using_allocator(static_cast<T *>(block), allocator,
                                                            std::forward<Args>(args)...);
      } else {
        void *block = allocateWithNode<T>();
        T *object = std::uninitialized_construct_using_allocator(static_cast<T *>(block), allocator,
                                                                 std::forward<Args>(args)...);
        remember<T>(block);
        return object;
      }
    }

    // Uninitialized room for count elements.
    template<typename T = uint8_t>
    [[nodiscard]] std::span<T> allocateSpan(size_t count, size_t alignment = alignof(T)) {
      if (count == 0) [[unlikely]] {
        return {};
      }
      if (count > SIZE_MAX / sizeof(T)) [[unlikely]] {
        throw std::bad_alloc();
      }

      void *ptr = allocate(count * sizeof(T), alignment);

      return std::span<T>(static_cast<T *>(ptr), count);
    }

    // Destroys the objects from make() and starts over in the first block,
    // which is kept. Every other block goes back to the upstream.
    void reset() noexcept;

    // Destroys the objects from make() and gives every block back to the
    // upstream. Only a caller buffer is kept.
    void release() noexcept;

    [[nodiscard]] static bool checkLockFree() { return std::atomic<BlockRange>{}.is_lock_free(); }

  private:
    void *do_allocate(size_t bytes, size_t alignment) override { return allocate(bytes, alignment); }

    void do_deallocate(void *, size_t, size_t) override {}

    [[nodiscard]] bool do_is_equal(const memory_resource &other) const noexcept override { return this == &other; }

    // Room for an object and the node behind it.
    template<typename T>
    void *allocateWithNode() {
      return allocate(nodeOffset<T> + sizeof(CleanupNode), std::max(alignof(T), alignof(CleanupNode)));
    }

    // Puts the object of a block from allocateWithNode() on the list. Only an
    // object that exists is remembered.
    template<typename T>
    void remember(void *block) noexcept {
      auto *node = new (static_cast<std::byte *>(block) + nodeOffset<T>)
          CleanupNode{.destroy =
                          [](CleanupNode *n) noexcept {
                            void *start = reinterpret_cast<std::byte *>(n) - nodeOffset<T>;
                            std::launder(static_cast<T *>(start))->~T();
                          },
                      .next = m_cleanup.load(std::memory_order_relaxed)};
      while (!m_cleanup.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed)) {
        ;
      }
    }

    // Newest first. Runs again for what a destructor created meanwhile.
    void runCleanups() noexcept {
      while (CleanupNode *node = m_cleanup.exchange(nullptr, std::memory_order_acq_rel)) {
        while (node != nullptr) {
          // The destructor may not touch the node, but read it first anyway.
          CleanupNode *next = node->next;
          node->destroy(node);
          node = next;
        }
      }
    }

    // Start of the allocation inside the range, or nullptr if it does not fit.
    // Compares sizes instead of pointers, because current + bytes can wrap
    // around for huge requests.
    [[nodiscard]] static std::byte *fit(const BlockRange &range, size_t bytes, size_t alignment) noexcept {
      if (!range.current) {
        return nullptr;
      }
      auto curr_addr = reinterpret_cast<uintptr_t>(range.current);
      auto end_addr = reinterpret_cast<uintptr_t>(range.end);
      uintptr_t aligned_addr = (curr_addr + alignment - 1) & ~(alignment - 1);
      if (aligned_addr < curr_addr || aligned_addr > end_addr || bytes > end_addr - aligned_addr) {
        return nullptr;
      }
      return range.current + (aligned_addr - curr_addr);
    }

    // New blocks are only aligned to max_align_t, so a stricter request can
    // lose up to alignment bytes of a fresh block to padding.
    [[nodiscard]] bool fits_new_block(size_t bytes, size_t alignment) const noexcept {
      if (bytes >= m_next_block_size) {
        return false;
      }
      return alignment <= alignof(std::max_align_t) || alignment <= m_next_block_size - bytes;
    }

    // Gets a block from the upstream and records it for release().
    std::byte *allocate_block(size_t size, size_t alignment);
    void handle_full_block(size_t bytes, size_t alignment);
    void *allocate_custom_block(size_t size, size_t alignment);
    void allocate_new_block(size_t size);
  };
} // namespace sc
