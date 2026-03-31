//
// Created by oleub on 05.03.26.
//

#pragma once
#include <memory>
#include <memory_resource>
#include <span>
#include <atomic>

#include "chaos_monotonic_resource.hpp"

namespace SC {
  class ChaosBumpArena {
    struct CleanupNode {
      void (*destroyer)(void *);

      void *object;
      CleanupNode *next;
    };

    static inline std::atomic<uint64_t> m_idGenerator{0};
    uint64_t m_id;

  public:
    explicit ChaosBumpArena(size_t size = 8 * 1024);

    ChaosBumpArena(const ChaosBumpArena &) = delete;

    ChaosBumpArena &operator=(const ChaosBumpArena &) = delete;

    std::pmr::memory_resource *resource() { return &m_pool; }

    template<typename T, typename... Args>
    T *make(Args &&... args) {
      void *mem = allocate(sizeof(T), alignof(T));
      T *obj = new(mem) T(std::forward<Args>(args)...);

      if constexpr (!std::is_trivially_destructible_v<T>) {
        void *nodeMem = allocate(sizeof(CleanupNode), alignof(CleanupNode));
        auto *newNode = new(nodeMem) CleanupNode{
          .destroyer = [](void *p) { static_cast<T *>(p)->~T(); },
          .object = obj,
          .next = nullptr
        };

        newNode->next = m_cleanupHead.load(std::memory_order_relaxed);
        while (!m_cleanupHead.compare_exchange_weak(newNode->next, newNode,
                                                    std::memory_order_release,
                                                    std::memory_order_relaxed));
      }
      return obj;
    }

    template<typename T = uint8_t>
    [[nodiscard]] std::span<T> allocateSpan(size_t count, size_t alignment = alignof(T)) {
      if (count == 0) [[unlikely]] {
        return {};
      }

      void *ptr = allocate(count * sizeof(T), alignment);

      return std::span<T>(static_cast<T *>(ptr), count);
    }

    std::string_view utf16ToUtf8(std::span<const char16_t> input);

    void *allocate(size_t size, size_t align);

    void reset();

  private :
    std::atomic<CleanupNode *> m_cleanupHead = nullptr;
    const size_t m_capacity;
    std::unique_ptr<uint8_t[]> m_backingBuffer;
    ChaosMonotonicResource m_pool;
  };
} // SC
