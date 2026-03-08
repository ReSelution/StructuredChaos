//
// Created by oleub on 05.03.26.
//

#pragma once
#include <memory>
#include <memory_resource>
#include <span>
#include <atomic>

namespace SC {
  class ChaosSlab {
    struct CleanupNode {
      void (*destroyer)(void *);

      void *object;
      CleanupNode *next;
    };

    static inline std::atomic<uint64_t> m_idGenerator{0};
    uint64_t m_id;

  public:
    explicit ChaosSlab(size_t size = 8 * 1024);

    ChaosSlab(const ChaosSlab &) = delete;

    ChaosSlab &operator=(const ChaosSlab &) = delete;

    std::pmr::memory_resource *resource() { return &m_pool; }

    template<typename T, typename... Args>
    T *make(Args &&... args) {
      void *mem = internal_allocate(sizeof(T), alignof(T));
      T *obj = new(mem) T(std::forward<Args>(args)...);

      if constexpr (!std::is_trivially_destructible_v<T>) {
        void *nodeMem = internal_allocate(sizeof(CleanupNode), alignof(CleanupNode));
        m_cleanupHead = new(nodeMem) CleanupNode{
          .destroyer = [](void *p) { static_cast<T *>(p)->~T(); },
          .object = obj,
          .next = m_cleanupHead
        };
      }
      return obj;
    }

    template<typename T = uint8_t>
    [[nodiscard]] std::span<T> allocateSpan(size_t count, size_t alignment = alignof(T)) {
      if (count == 0) [[unlikely]] {
        return {};
      }

      void *ptr = internal_allocate(count * sizeof(T), alignment);

      return std::span<T>(static_cast<T *>(ptr), count);
    }

    std::string_view utf16ToUtf8(std::span<const char16_t> input);

    void reset();

  private :
    void *internal_allocate(size_t size, size_t align);

    CleanupNode *m_cleanupHead = nullptr;
    const size_t m_capacity;
    std::unique_ptr<uint8_t[]> m_backingBuffer;
    std::pmr::monotonic_buffer_resource m_pool;
  };
} // SC
