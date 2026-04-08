#pragma once

#include <array>
#include <unordered_map>

#include "threading/chaos_spin_lock.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace SC {
  /**
   * @brief ChaosShardedCache - A thread-safe concurrent hash map.
   * Reduces lock contention by splitting data into multiple shards.
   * * @tparam K Key type (should be convertible to size_t for hashing).
   * @tparam V Value type.
   * @tparam ShardCount Number of shards (defaults to 64).
   */
  template<typename K, typename V, size_t ShardCount = 64>
    requires std::integral<K>
  class ChaosShardedCache {
    struct IdentityHash {
      size_t operator()(size_t key) const noexcept { return key; }
    };

    /**
     * @brief Individual shard protected by a spinlock.
     * Aligned to 64 bytes to prevent "False Sharing".
     */
    struct alignas(64) Shard {
      ChaosSpinLock lock;
      std::unordered_map<K, V, IdentityHash> data;

      void acquire() {
        lock.lock();
      }

      void release() {
        lock.unlock();
      }
    };

    std::array<Shard, ShardCount> shards;

    /**
     * @brief Maps a key to a specific shard.
     */
    Shard &get_shard(const K &key) {
      return shards[static_cast<size_t>(key) % ShardCount];
    }

  public:
    /**
     * @brief Constructs the memory with an optional initial capacity.
     * @param totalInitialCapacity Pre-allocates memory for the whole memory.
     */
    explicit ChaosShardedCache(size_t totalInitialCapacity = 0) {
      if (totalInitialCapacity > 0) {
        reserve(totalInitialCapacity);
      }
    }

    /**
     * @brief Pre-allocates space in each shard.
     */
    void reserve(size_t totalSize) {
      const size_t shardSize = totalSize / ShardCount;
      for (auto &shard: shards) {
        shard.acquire();
        shard.data.reserve(shardSize);
        shard.release();
      }
    }

    /**
     * @brief Clears the memory.
     * @note Your original logic assumed V is a weak_ptr to an object with a reset() method.
     */
    void reset() {
      for (auto &shard: shards) {
        shard.acquire();
        for (auto &[key, value]: shard.data) {
          // Assuming V is a weak_ptr or similar wrapper
          if (auto locked = value.lock()) {
            locked->reset();
          }
        }
        shard.data.clear();
        shard.release();
      }
    }

    ~ChaosShardedCache() {
      reset();
    }

    /**
     * @brief Retrieves a value. Returns a default-constructed V if not found.
     */
    V get(const K &key) {
      auto &shard = get_shard(key);
      shard.acquire();
      auto it = shard.data.find(key);
      V result = (it != shard.data.end()) ? it->second : V{};
      shard.release();
      return result;
    }

    /**
     * @brief Inserts or updates a value in the memory.
     */
    void insert(const K &key, V value) {
      auto &shard = get_shard(key);
      shard.acquire();
      shard.data[key] = std::move(value);
      shard.release();
    }
  };
} // namespace SC
