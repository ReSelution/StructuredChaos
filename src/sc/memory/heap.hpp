#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

#include <mimalloc.h>

#include "sc/memory/cleanup_list.hpp"
#include "sc/memory/pmr_traits.hpp"

namespace sc {

  // Owns a mimalloc heap. Blocks need not be freed one by one: destroying or
  // resetting the heap frees them all at once.
  //
  // Objects created with make() or makePmr() are destroyed then, the newest
  // first. Nothing else is: what lies in a block from allocate() or
  // allocateSpan() gets no destructor call. Neither does a pmr container from
  // makePmr() that keeps everything in this heap, since its memory goes with
  // the heap anyway.
  //
  // Is a std::pmr::memory_resource, so that pmr containers can live in it. What
  // they deallocate is given back to the heap right away.
  //
  // Allocating and make() are safe from several threads at the same time.
  // reset() and the destructor must not run while another thread still does
  // either.
  //
  // Cannot be copied or moved: pmr containers keep the address of the heap as
  // their resource.
  class Heap final : public std::pmr::memory_resource {
  public:
    Heap() : m_heap(create()) {}

    Heap(const Heap &) = delete;

    Heap &operator=(const Heap &) = delete;

    ~Heap() override { destroy(); }

    // The mimalloc heap, for the mi_heap_* functions.
    [[nodiscard]] mi_heap_t *get() const noexcept { return m_heap; }

    // For an alignment that is only known at run time. Hides the function of
    // the same name in memory_resource, which would do the same through a
    // virtual call.
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    [[nodiscard]] void *allocate(size_t size, size_t align = alignof(std::max_align_t)) {
      assert(std::has_single_bit(align) && "the alignment has to be a power of two");
      return checked(align <= alignof(std::max_align_t) ? mi_heap_malloc(m_heap, size)
                                                        : mi_heap_malloc_aligned(m_heap, size, align));
    }

    // For an alignment that is known at compile time, which decides there
    // whether the aligned allocation of mimalloc is needed.
    template<size_t Align>
    [[nodiscard]] void *allocate(size_t size) {
      static_assert(Align != 0 && (Align & (Align - 1)) == 0, "the alignment has to be a power of two");
      if constexpr (Align <= alignof(std::max_align_t)) {
        return checked(mi_heap_malloc(m_heap, size));
      } else {
        return checked(mi_heap_malloc_aligned(m_heap, size, Align));
      }
    }

    // The object lives until the heap is reset or destroyed. Its destructor
    // is run then, if it has one. For a pmr container that is to live in this
    // heap, use makePmr().
    template<typename T, typename... Args>
    T *make(Args &&...args) {
      if constexpr (std::is_trivially_destructible_v<T>) {
        return new (allocate<alignof(T)>(sizeof(T))) T(std::forward<Args>(args)...);
      } else {
        void *block = allocateWithNode<T>();
        T *object = construct<T>(block, std::forward<Args>(args)...);
        m_cleanup.remember<T>(block);
        return object;
      }
    }

    // Like make(), for a type that takes a pmr allocator: the heap passes
    // itself as that allocator, behind or in front of args as the type wants
    // it. The object and everything it allocates lives in the heap, so a
    // container whose elements own nothing else needs no destructor call.
    template<typename T, typename... Args>
      requires std::uses_allocator_v<T, std::pmr::polymorphic_allocator<>>
    T *makePmr(Args &&...args) {
      const std::pmr::polymorphic_allocator<> allocator(this);
      if constexpr (internal::ownsOnlyResourceMemory<T>) {
        void *block = allocate<alignof(T)>(sizeof(T));
        return constructPmr<T>(block, allocator, std::forward<Args>(args)...);
      } else {
        void *block = allocateWithNode<T>();
        T *object = constructPmr<T>(block, allocator, std::forward<Args>(args)...);
        m_cleanup.remember<T>(block);
        return object;
      }
    }

    // Uninitialized room for count elements.
    template<typename T = uint8_t>
    [[nodiscard]] std::span<T> allocateSpan(size_t count) {
      if (count == 0) [[unlikely]] {
        return {};
      }

      void *ptr = allocate<alignof(T)>(byteCount<T>(count));

      return std::span<T>(static_cast<T *>(ptr), count);
    }

    // The same with an alignment stricter than the one of the type.
    template<typename T = uint8_t>
    [[nodiscard]] std::span<T> allocateSpan(size_t count, size_t alignment) {
      if (count == 0) [[unlikely]] {
        return {};
      }

      void *ptr = allocate(byteCount<T>(count), alignment);

      return std::span<T>(static_cast<T *>(ptr), count);
    }

    [[nodiscard]] bool contains(const void *ptr) const noexcept {
      return m_heap != nullptr && mi_heap_contains(m_heap, ptr);
    }

    // Destroys the objects from make() and frees everything that was
    // allocated from the heap. The new heap is created first: if that fails,
    // nothing has changed.
    void reset() {
      mi_heap_t *fresh = create();
      destroy();
      m_heap = fresh;
    }

  private:
    void *do_allocate(size_t bytes, size_t alignment) override { return allocate(bytes, alignment); }

    void do_deallocate(void *ptr, size_t, size_t) override { mi_free(ptr); }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
      return this == &other;
    }

    // Room for an object and the node behind it.
    template<typename T>
    void *allocateWithNode() {
      return allocate<internal::CleanupList::blockAlignment<T>>(internal::CleanupList::blockSize<T>);
    }

    // Gives the block back if the constructor throws.
    template<typename T, typename... Args>
    static T *construct(void *block, Args &&...args) {
      try {
        return new (block) T(std::forward<Args>(args)...);
      } catch (...) {
        mi_free(block);
        throw;
      }
    }

    template<typename T, typename... Args>
    static T *constructPmr(void *block, const std::pmr::polymorphic_allocator<> &allocator, Args &&...args) {
      try {
        return std::uninitialized_construct_using_allocator(static_cast<T *>(block), allocator,
                                                            std::forward<Args>(args)...);
      } catch (...) {
        mi_free(block);
        throw;
      }
    }

    static void *checked(void *ptr) {
      if (ptr == nullptr) [[unlikely]] {
        throw std::bad_alloc();
      }
      return ptr;
    }

    template<typename T>
    static size_t byteCount(size_t count) {
      if (count > SIZE_MAX / sizeof(T)) [[unlikely]] {
        throw std::bad_alloc();
      }
      return count * sizeof(T);
    }

    static mi_heap_t *create() {
      mi_heap_t *heap = mi_heap_new();
      if (heap == nullptr) [[unlikely]] {
        throw std::bad_alloc();
      }
      return heap;
    }

    void destroy() noexcept {
      m_cleanup.runAll();
      if (m_heap != nullptr) {
        mi_heap_destroy(m_heap);
        m_heap = nullptr;
      }
    }

    mi_heap_t *m_heap;
    internal::CleanupList m_cleanup;
  };
} // namespace sc
