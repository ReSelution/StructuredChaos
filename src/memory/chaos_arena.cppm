module;

#include <atomic>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <span>

export module SC.Memory:Arena;

import :MRes;

namespace SC {

export template <typename T, typename = void>
struct is_pmr_container : std::false_type {};

export template <typename T>
struct is_pmr_container<T, std::void_t<typename T::allocator_type>> {
  static constexpr bool value =
      std::is_same_v<typename T::allocator_type,
                     std::pmr::polymorphic_allocator<typename T::value_type>>;
};

export template <typename T>
inline constexpr bool is_pmr_container_v = is_pmr_container<T>::value;

export class ChaosArena {
  struct CleanupNode {
    void (*destroyer)(void *);
    void *object;
    CleanupNode *next;
  };

public:
  explicit ChaosArena(size_t size = 8 * 1024)
      : m_capacity(size),
        m_backingBuffer(std::make_unique_for_overwrite<uint8_t[]>(m_capacity)),
        m_pool(m_backingBuffer.get(), m_capacity) {}

  ChaosArena(const ChaosArena &) = delete;
  ChaosArena &operator=(const ChaosArena &) = delete;

  [[nodiscard]] std::pmr::memory_resource *resource() noexcept {
    return &m_pool;
  }

  template <typename T, typename... Args> T *make(Args &&...args) {
    void *mem = allocate(sizeof(T), alignof(T));
    T *obj = new (mem) T(std::forward<Args>(args)...);

    if constexpr (!std::is_trivially_destructible_v<T>) {
      void *nodeMem = allocate(sizeof(CleanupNode), alignof(CleanupNode));
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
    if (count == 0) [[unlikely]]
      return {};
    void *ptr = allocate(count * sizeof(T), alignment);
    return std::span<T>(static_cast<T *>(ptr), count);
  }

  [[nodiscard]] void *allocate(size_t size, size_t align) {
    return m_pool.allocate(size, align);
  }

  void reset() noexcept {
    CleanupNode *head =
        m_cleanupHead.exchange(nullptr, std::memory_order_acq_rel);
    while (head) {
      head->destroyer(head->object);
      head = head->next;
    }
    m_pool.release();
  }

private:
  std::atomic<CleanupNode *> m_cleanupHead{nullptr};
  const size_t m_capacity;
  std::unique_ptr<uint8_t[]> m_backingBuffer;
  ChaosMonotonicResource m_pool;
};

} // namespace SC
