#pragma once
#include <memory_resource>
#include <utility>

namespace SC {
  /**
   * @brief ChaosThreadCache - A template-based thread-local memory pool.
   * @tparam BufferSize Size of the pre-allocated buffer in bytes.
   */
  template<size_t BufferSize = 8 * 1024>
  class ChaosTLSSlab {
  public:
    struct Storage {
      char buffer[BufferSize];
      std::pmr::monotonic_buffer_resource pool;

      // Falls fallback to heap is needed, we use new_delete_resource
      Storage() : buffer{}, pool(buffer, BufferSize) {
      }

      Storage(const Storage &) = delete;

      Storage &operator=(const Storage &) = delete;
    };

  private:
    static Storage &get_storage() {
      thread_local Storage storage;
      return storage;
    }

  public:
    /**
     * @brief Allocates and constructs an object in the cache.
     */
    template<typename T, typename... Args>
    [[nodiscard]] static T *make(Args &&... args) {
      void *mem = get_storage().pool.allocate(sizeof(T), alignof(T));
      return new(mem) T(std::forward<Args>(args)...);
    }

    /**
     * @brief Returns the PMR resource pointer.
     */
    static std::pmr::memory_resource *resource() {
      return &get_storage().pool;
    }

    /**
     * @brief Resets the pool by releasing all allocated memory.
     */
    static void reset() {
      get_storage().pool.release();
    }

    /**
     * @brief Returns the fixed buffer size of this cache instance.
     */
    static constexpr size_t capacity() {
      return BufferSize;
    }
  };

  using TLSCache = ChaosTLSSlab<>;
} //
