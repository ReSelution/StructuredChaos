#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>

#ifdef __linux__
#include <sys/mman.h>
#endif

namespace sc {

  // Upstream for large, long-lived blocks: gets them straight from the
  // operating system and asks for transparent huge pages. The first touch of
  // 2 MiB then costs one page fault instead of 512.
  //
  // Requests below 2 MiB, and everything outside of Linux, go to new and
  // delete.
  //
  // Needs a process that is allowed to use huge pages. That is why mimalloc is
  // built with MI_NO_THP and not with mi_allow_thp=off, which would disable
  // them for the whole process. If the THP mode of the system is "always"
  // instead of "madvise", the memory of mimalloc gets huge pages as well.
  class HugePageResource final : public std::pmr::memory_resource {
  public:
    static constexpr size_t HUGE_PAGE_SIZE = size_t{2} * 1024 * 1024;

    // One instance is enough: it has no state.
    static HugePageResource *get() {
      static HugePageResource instance;
      return &instance;
    }

  private:
    static constexpr size_t roundUp(size_t bytes) { return (bytes + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1); }

    // Whether a request is served by mmap. Has to give the same answer in
    // do_allocate() and do_deallocate().
    static constexpr bool isHuge(size_t bytes, size_t alignment) {
#ifdef __linux__
      return bytes >= HUGE_PAGE_SIZE && alignment <= HUGE_PAGE_SIZE;
#else
      (void) bytes;
      (void) alignment;
      return false;
#endif
    }

    void *do_allocate(size_t bytes, size_t alignment) override {
#ifdef __linux__
      if (isHuge(bytes, alignment)) {
        // Mapped one huge page larger, so that the start can be aligned to
        // 2 MiB. What sticks out on either side is given back.
        const size_t size = roundUp(bytes);
        const size_t mapped = size + HUGE_PAGE_SIZE;
        void *raw = mmap(nullptr, mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED) {
          throw std::bad_alloc();
        }
        auto *start = static_cast<std::byte *>(raw);
        const size_t lead = roundUp(reinterpret_cast<uintptr_t>(start)) - reinterpret_cast<uintptr_t>(start);
        std::byte *aligned = start + lead;
        if (lead != 0) {
          (void) munmap(start, lead);
        }
        if (const size_t tail = mapped - lead - size; tail != 0) {
          (void) munmap(aligned + size, tail);
        }
        // Without huge pages the block is still usable.
        (void) madvise(aligned, size, MADV_HUGEPAGE);
        return aligned;
      }
#endif
      return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void *ptr, size_t bytes, size_t alignment) override {
#ifdef __linux__
      if (isHuge(bytes, alignment)) {
        (void) munmap(ptr, roundUp(bytes));
        return;
      }
#endif
      std::pmr::new_delete_resource()->deallocate(ptr, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
      return this == &other;
    }
  };
} // namespace sc
