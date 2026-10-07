//
// Created by oleub on 05.03.26.
//

#pragma once

#include "sc/memory/monotonic_resource.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace sc {

class Arena {
  struct CleanupNode {
    void (*destroyer)(void *);

    void *object;
    CleanupNode *next;
  };

public:
  explicit Arena(size_t size = 8 * 1024);

  Arena(const Arena &) = delete;

  Arena &operator=(const Arena &) = delete;

  ~Arena() { reset(); }

  std::pmr::memory_resource *resource() { return &m_pool; }

  template <typename T, typename... Args> T *make(Args &&...args) {
    // Allocated before the object exists: if this throws, nothing is left
    // behind that would need its destructor run.
    void *nodeMem = nullptr;
    if constexpr (!std::is_trivially_destructible_v<T>) {
      nodeMem = allocate(sizeof(CleanupNode), alignof(CleanupNode));
    }

    void *mem = allocate(sizeof(T), alignof(T));
    T *obj = new (mem) T(std::forward<Args>(args)...);

    if constexpr (!std::is_trivially_destructible_v<T>) {
      auto *newNode = new (nodeMem)
          CleanupNode{.destroyer = [](void *p) { static_cast<T *>(p)->~T(); },
                      .object = obj,
                      .next = nullptr};

      newNode->next = m_cleanupHead.load(std::memory_order_relaxed);
      while (!m_cleanupHead.compare_exchange_weak(newNode->next, newNode,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed))
        ;
    }
    return obj;
  }

  template <typename T = uint8_t>
  [[nodiscard]] std::span<T> allocateSpan(size_t count,
                                          size_t alignment = alignof(T)) {
    if (count == 0) [[unlikely]] {
      return {};
    }

    void *ptr = allocate(count * sizeof(T), alignment);

    return std::span<T>(static_cast<T *>(ptr), count);
  }

  // Null-terminated UTF-8 copy of input. Empty if input is not valid UTF-16.
  std::string_view utf16ToUtf8(std::span<const char16_t> input);

  void *allocate(size_t size, size_t align);

  void reset();

private:
  std::atomic<CleanupNode *> m_cleanupHead = nullptr;
  const size_t m_capacity;
  std::unique_ptr<uint8_t[]> m_backingBuffer;
  MonotonicResource m_pool;
};
} // namespace sc
