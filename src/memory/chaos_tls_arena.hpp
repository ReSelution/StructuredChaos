#pragma once
#include <memory_resource>
#include <utility>
#include <memory>

namespace SC {
  /**
   * @brief ChaosThreadCache - A template-based thread-local memory pool.
   * @tparam BufferSize Size of the pre-allocated buffer in bytes.
   */
  template<size_t BufferSize = 64 * 1024>
  class ChaosTLSArena {
  public:
    struct Storage {
      std::unique_ptr<uint8_t[]> buffer;
      std::pmr::monotonic_buffer_resource pool;

      Storage() : buffer(std::make_unique_for_overwrite<uint8_t[]>(BufferSize)),
                  pool(buffer.get(), BufferSize, std::pmr::get_default_resource()) {
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
     * @brief Allocates and constructs an object in the memory.
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
     * @brief Returns the fixed buffer size of this memory instance.
     */
    static constexpr size_t capacity() {
      return BufferSize;
    }
  };

  using TLSCache = ChaosTLSArena<>;
} //
