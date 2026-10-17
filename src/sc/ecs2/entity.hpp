#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <type_traits>
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

  // No entity has this index or a higher one, so there are never more
  // entities than this at a time. It is what every column reserves addresses
  // for.
  inline constexpr uint32_t INDEX_LIMIT = uint32_t{1} << 24;

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

  // Says that an entity is unlike most others: world.create(rare). Such
  // entities get their indices from the other end, where they are among
  // themselves. A component that only they have then needs memory for their
  // few pages and not for pages all over the world.
  struct Rare {};
  inline constexpr Rare rare{};

} // namespace sc::ecs2

namespace sc::ecs2::internal {

  // Hands out entities and knows which ones exist.
  //
  // The indices come from both ends: from zero upwards for most entities and
  // from INDEX_LIMIT downwards for the rare ones, a page at a time, until the
  // two meet. Every thread has a page of its own to take new indices from,
  // and takes another one when that is used up.
  //
  // create and valid are safe from any thread at any time. The threads of the
  // pool and the thread that set it up need no lock for it, every other thread
  // takes turns with the others of its kind for a new index. destroy must have
  // nothing running next to it.
  class EntityAllocator {
    // The generations of this many indices are kept in one piece.
    static constexpr unsigned BLOCK_BITS = 16;
    static constexpr uint32_t BLOCK_SIZE = uint32_t{1} << BLOCK_BITS;
    static constexpr size_t BLOCK_COUNT = INDEX_LIMIT >> BLOCK_BITS;
    static constexpr uint32_t PAGE_COUNT = INDEX_LIMIT >> PAGE_BITS;

    // The indices a thread hands out next, which nobody else has.
    struct alignas(64) Share {
      uint32_t next = 0;
      uint32_t end = 0;
      // And for the rare entities.
      uint32_t rareNext = 0;
      uint32_t rareEnd = 0;
    };

    // The indices destroy gave back, to be handed out again.
    struct FreeList {
      // How many of the indices are left to hand out, from the back. Below
      // zero when several threads went for the last one.
      alignas(64) std::atomic<int64_t> left{0};
      // Only changed by give, so take reads it without a lock.
      std::vector<uint32_t> indices;

      // False if there is none.
      [[nodiscard]] bool take(uint32_t &index) noexcept {
        // Read first: once nothing is left, no thread writes this any more.
        if (left.load(std::memory_order_acquire) <= 0) {
          return false;
        }
        const int64_t before = left.fetch_sub(1, std::memory_order_acq_rel);
        if (before <= 0) {
          return false;
        }
        index = indices[static_cast<size_t>(before - 1)];
        return true;
      }

      // take for up to count indices with one fetch_sub: those of the span,
      // in the order destroy gave them back. Empty if there is none.
      [[nodiscard]] std::span<const uint32_t> takeMany(size_t count) noexcept {
        if (count == 0 || left.load(std::memory_order_acquire) <= 0) {
          return {};
        }
        const int64_t before = left.fetch_sub(static_cast<int64_t>(count), std::memory_order_acq_rel);
        if (before <= 0) {
          return {};
        }
        const auto taken = static_cast<size_t>(std::min(before, static_cast<int64_t>(count)));
        return {indices.data() + (static_cast<size_t>(before) - taken), taken};
      }

      // Must have nothing running next to it.
      void give(uint32_t index) {
        open();
        add(index);
        close();
      }

      // give for many indices: open, add each, close. Must have nothing
      // running next to it from open to close.
      void open() {
        // What take took since the last time.
        indices.resize(static_cast<size_t>(std::max<int64_t>(left.load(std::memory_order_relaxed), 0)));
      }
      void add(uint32_t index) { indices.push_back(index); }
      void close() { left.store(static_cast<int64_t>(indices.size()), std::memory_order_release); }
    };

  public:
    static constexpr size_t MAX_THREADS = 128;

    EntityAllocator() : m_blocks(std::make_unique<std::atomic<uint32_t *>[]>(BLOCK_COUNT)) {}

    ~EntityAllocator() {
      for (size_t block = 0; block < BLOCK_COUNT; ++block) {
        mi_free(m_blocks[block].load(std::memory_order_relaxed));
      }
    }

    EntityAllocator(const EntityAllocator &) = delete;
    EntityAllocator &operator=(const EntityAllocator &) = delete;

    // An index that was given back by destroy, or else the next one of the
    // calling thread. Throws std::length_error if all indices below
    // INDEX_LIMIT are taken.
    [[nodiscard]] Entity create() {
      if (uint32_t index = 0; m_free.take(index)) {
        return makeEntity(index, *generation(index));
      }

      return withShare([this](Share &share) {
        if (share.next == share.end) [[unlikely]] {
          share.next = makePage(false);
          share.end = share.next + PAGE_SIZE;
        }
        // Never handed out before.
        return makeEntity(share.next++, 0);
      });
    }

    // An entity for every place of the span, as create makes them. Calls the
    // function with every run of indices it handed out, as (first index,
    // count): what destroy gave back in the rows it lies in, the rest in rows
    // as long as the pages of the thread allow. For a thread outside the pool
    // the function is called under the lock of withShare.
    //
    // The indices given back are taken all at once, and every row of them
    // goes to the function in one call: one by one, a column made every one
    // of them on its own (count, mask and summary per entity), 14.5 instead
    // of 1.3 ns per entity with two components for indices that destroy had
    // given back in a row (BM_CreateEntitiesInBulk of ecs_benchmark, gcc);
    // in rows 2.1 ns at 1024 entities and 6.3 at 262144.
    template<typename Function>
    void create(std::span<Entity> entities, Function &&run) {
      size_t made = 0;
      const std::span<const uint32_t> reused = m_free.takeMany(entities.size());
      for (size_t i = 0; i < reused.size();) {
        const uint32_t first = reused[i];
        uint32_t count = 1;
        while (i + count < reused.size() && reused[i + count] == first + count) {
          ++count;
        }
        for (uint32_t k = 0; k < count; ++k) {
          entities[made + k] = makeEntity(first + k, *generation(first + k));
        }
        run(first, count);
        made += count;
        i += count;
      }

      if (made == entities.size()) {
        return;
      }
      withShare([&](Share &share) {
        while (made < entities.size()) {
          if (share.next == share.end) {
            share.next = makePage(false);
            share.end = share.next + PAGE_SIZE;
          }
          const auto count = static_cast<uint32_t>(std::min<size_t>(entities.size() - made, share.end - share.next));
          for (uint32_t i = 0; i < count; ++i) {
            entities[made + i] = makeEntity(share.next + i, 0);
          }
          run(share.next, count);
          share.next += count;
          made += count;
        }
      });
    }

    // The same from the other end: an index among those of the rare entities.
    [[nodiscard]] Entity create(Rare) {
      if (uint32_t index = 0; m_rareFree.take(index)) {
        return makeEntity(index, *generation(index));
      }

      return withShare([this](Share &share) {
        if (share.rareNext == share.rareEnd) [[unlikely]] {
          share.rareNext = makePage(true);
          share.rareEnd = share.rareNext + PAGE_SIZE;
        }
        return makeEntity(share.rareNext++, 0);
      });
    }

    [[nodiscard]] bool valid(Entity entity) const noexcept {
      const uint32_t index = indexOf(entity);
      const Pages pages = this->pages();
      if (index >= pages.limit() && (index < pages.rareBegin() || index >= INDEX_LIMIT)) {
        return false;
      }
      const uint32_t *current = generation(index);
      return current != nullptr && *current == generationOf(entity);
    }

    // The entity must exist. Its index is handed out again with another
    // generation, so that the entity stays invalid.
    void destroy(Entity entity) {
      const uint32_t index = indexOf(entity);
      ++*generation(index);
      // Back to where it came from.
      (index >= pages().rareBegin() ? m_rareFree : m_free).give(index);
    }

    // destroy for every entity that exists: one given twice or no longer
    // there is left out. Calls the function with the index of each one it
    // destroys and returns how many those were. The indices go back to their
    // lists in one step each.
    template<typename Function>
    size_t destroyMany(std::span<const Entity> entities, Function &&function) {
      const uint32_t rareBegin = pages().rareBegin();
      m_free.open();
      m_rareFree.open();
      size_t destroyed = 0;
      for (const Entity entity: entities) {
        if (!valid(entity)) {
          continue;
        }
        const uint32_t index = indexOf(entity);
        ++*generation(index);
        (index >= rareBegin ? m_rareFree : m_free).add(index);
        function(index);
        ++destroyed;
      }
      m_free.close();
      m_rareFree.close();
      return destroyed;
    }

    // Which pages have entities: the first so many, and the last so many for
    // the rare ones.
    struct Pages {
      uint32_t common = 0;
      uint32_t rare = 0;

      // No common entity has this index or a higher one.
      [[nodiscard]] uint32_t limit() const noexcept { return common << PAGE_BITS; }
      // No rare entity has an index below this.
      [[nodiscard]] uint32_t rareBegin() const noexcept { return INDEX_LIMIT - (rare << PAGE_BITS); }
    };

    [[nodiscard]] Pages pages() const noexcept {
      const uint64_t counts = m_pageCounts.load(std::memory_order_acquire);
      return {.common = static_cast<uint32_t>(counts), .rare = static_cast<uint32_t>(counts >> 32)};
    }

    // The generation of an index, followed by those of the indices after it up
    // to the end of their page. Null if no entity ever had an index near it.
    [[nodiscard]] uint32_t *generation(uint32_t index) const noexcept {
      uint32_t *block = m_blocks[index >> BLOCK_BITS].load(std::memory_order_acquire);
      return block != nullptr ? block + (index & (BLOCK_SIZE - 1)) : nullptr;
    }

  private:
    // Calls the function with the Share of the calling thread. The threads
    // without one of their own, those outside the pool (threading::NO_THREAD_ID)
    // and pool threads beyond MAX_THREADS, use one together, one at a time.
    template<typename Function>
    std::invoke_result_t<Function &, Share &> withShare(Function &&function) {
      const uint32_t thread = threading::threadId();
      if (thread < MAX_THREADS) [[likely]] {
        return function(m_shares[thread]);
      }
      const std::scoped_lock lock{m_sharedMutex};
      return function(m_shared);
    }

    // The first index of a page no thread has indices of yet: the next one
    // from below, or from above for the rare entities.
    uint32_t makePage(bool rare) {
      uint64_t counts = m_pageCounts.load(std::memory_order_relaxed);
      uint32_t first = 0;
      do {
        const auto common = static_cast<uint32_t>(counts);
        const auto rares = static_cast<uint32_t>(counts >> 32);
        if (common + rares >= PAGE_COUNT) [[unlikely]] {
          throw std::length_error("sc::ecs2: no entity index is left");
        }
        first = rare ? INDEX_LIMIT - ((rares + 1) << PAGE_BITS) : common << PAGE_BITS;
      } while (!m_pageCounts.compare_exchange_weak(counts, counts + (rare ? uint64_t{1} << 32 : uint64_t{1}),
                                                   std::memory_order_acq_rel, std::memory_order_relaxed));
      makeBlock(first);
      return first;
    }

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
    // How many pages were handed out to threads: from below in the lower half,
    // from above in the upper half. Both in one, so that they never overlap.
    alignas(64) std::atomic<uint64_t> m_pageCounts{0};
    FreeList m_free;
    FreeList m_rareFree;
    std::array<Share, MAX_THREADS> m_shares{};
    // For the threads that have none in m_shares, see withShare.
    Share m_shared;
    std::mutex m_sharedMutex;
  };

} // namespace sc::ecs2::internal
