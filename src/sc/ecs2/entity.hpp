#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <mimalloc.h>

#include "sc/threading/threading.hpp"

namespace sc::ecs2 {

  // An entity: the index its components are stored at and, above it, the
  // generation of that index, which tells the entity apart from earlier ones
  // that had the same index.
  //
  // The bits are split up here and nowhere else. The upper bits of the index
  // are meant to name a room later, a range of indices that belongs to one
  // system or thread.
  enum class Entity : uint64_t {};

  inline constexpr unsigned INDEX_BITS = 32;

  // Components are stored in pages of this many entities. Threads that work
  // on different pages share nothing.
  inline constexpr unsigned PAGE_BITS = 12;
  inline constexpr uint32_t PAGE_SIZE = uint32_t{1} << PAGE_BITS;

  [[nodiscard]] constexpr uint32_t indexOf(Entity entity) noexcept {
    return static_cast<uint32_t>(static_cast<uint64_t>(entity));
  }

  [[nodiscard]] constexpr uint32_t generationOf(Entity entity) noexcept {
    return static_cast<uint32_t>(static_cast<uint64_t>(entity) >> INDEX_BITS);
  }

  [[nodiscard]] constexpr Entity makeEntity(uint32_t index, uint32_t generation) noexcept {
    return Entity{(uint64_t{generation} << INDEX_BITS) | index};
  }

  // No entity has this value.
  inline constexpr Entity null{~uint64_t{0}};

} // namespace sc::ecs2

namespace sc::ecs2::internal {

  // Hands out entities and knows which ones exist.
  //
  // create and valid are safe from the threads of the pool and the thread that
  // set it up, at any time and without a lock. destroy must have nothing
  // running next to it.
  class EntityAllocator {
    // The generations of this many indices are kept in one piece.
    static constexpr unsigned BLOCK_BITS = 16;
    static constexpr uint32_t BLOCK_SIZE = uint32_t{1} << BLOCK_BITS;
    static constexpr size_t BLOCK_COUNT = size_t{1} << (INDEX_BITS - BLOCK_BITS);

    // The indices a thread hands out next: a page nobody else has indices of.
    struct alignas(64) Share {
      uint32_t next = 0;
      uint32_t end = 0;
    };

  public:
    static constexpr size_t MAX_THREADS = 128;

    EntityAllocator() : m_blocks(std::make_unique<std::atomic<uint32_t *>[]>(BLOCK_COUNT)) {}

    ~EntityAllocator() {
      const uint32_t limit = m_next.load(std::memory_order_relaxed);
      for (size_t block = 0; block <= (limit >> BLOCK_BITS); ++block) {
        mi_free(m_blocks[block].load(std::memory_order_relaxed));
      }
    }

    EntityAllocator(const EntityAllocator &) = delete;
    EntityAllocator &operator=(const EntityAllocator &) = delete;

    // An index that was given back by destroy, or else the next one of the
    // page of the calling thread. Only the latter are in pages no other
    // thread creates entities in.
    [[nodiscard]] Entity create() {
      // Read first: once nothing is left, no thread writes this any more.
      if (m_freeLeft.load(std::memory_order_acquire) > 0) {
        const int64_t left = m_freeLeft.fetch_sub(1, std::memory_order_acq_rel);
        if (left > 0) {
          const uint32_t index = m_free[static_cast<size_t>(left - 1)];
          return makeEntity(index, *generation(index));
        }
      }

      const auto thread = static_cast<size_t>(threading::thread_id());
      assert(thread < MAX_THREADS);
      Share &share = m_shares[thread];
      if (share.next == share.end) [[unlikely]] {
        share.next = m_next.fetch_add(PAGE_SIZE, std::memory_order_relaxed);
        share.end = share.next + PAGE_SIZE;
        makeBlock(share.next);
      }
      // Never handed out before.
      return makeEntity(share.next++, 0);
    }

    [[nodiscard]] bool valid(Entity entity) const noexcept {
      const uint32_t index = indexOf(entity);
      if (index >= m_next.load(std::memory_order_acquire)) {
        return false;
      }
      const uint32_t *current = generation(index);
      return current != nullptr && *current == generationOf(entity);
    }

    // The entity must exist. Its index is handed out again with another
    // generation, so that the entity stays invalid.
    void destroy(Entity entity) {
      // What create took since the last time.
      m_free.resize(static_cast<size_t>(std::max<int64_t>(m_freeLeft.load(std::memory_order_relaxed), 0)));
      const uint32_t index = indexOf(entity);
      ++*generation(index);
      m_free.push_back(index);
      m_freeLeft.store(static_cast<int64_t>(m_free.size()), std::memory_order_release);
    }

    // No entity has this index or a higher one.
    [[nodiscard]] uint32_t limit() const noexcept { return m_next.load(std::memory_order_acquire); }

    // The generation of an index, followed by those of the indices after it up
    // to the end of their page. Null if no entity ever had an index near it.
    [[nodiscard]] uint32_t *generation(uint32_t index) const noexcept {
      uint32_t *block = m_blocks[index >> BLOCK_BITS].load(std::memory_order_acquire);
      return block != nullptr ? block + (index & (BLOCK_SIZE - 1)) : nullptr;
    }

  private:
    // Makes sure the generations of the index and its page have memory.
    void makeBlock(uint32_t index) {
      std::atomic<uint32_t *> &slot = m_blocks[index >> BLOCK_BITS];
      if (slot.load(std::memory_order_acquire) != nullptr) [[likely]] {
        return;
      }
      auto *fresh = static_cast<uint32_t *>(mi_zalloc_aligned(BLOCK_SIZE * sizeof(uint32_t), 64));
      uint32_t *expected = nullptr;
      if (!slot.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel)) {
        // Another thread was faster.
        mi_free(fresh);
      }
    }

    // One for every index block, null until an index of it is handed out.
    std::unique_ptr<std::atomic<uint32_t *>[]> m_blocks;
    // The first index no thread has in its share yet. Always the first of a page.
    alignas(64) std::atomic<uint32_t> m_next{0};
    // How many of m_free are left to hand out, from the back. Below zero when
    // several threads went for the last one.
    alignas(64) std::atomic<int64_t> m_freeLeft{0};
    // Only changed by destroy, so create reads it without a lock.
    std::vector<uint32_t> m_free;
    std::array<Share, MAX_THREADS> m_shares{};
  };

} // namespace sc::ecs2::internal
