#include "ring_queue.hpp"

#include <new>

#ifdef _WIN32
// Without it windows.h defines min and max as macros.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace sc::threading::internal {
  namespace {
    size_t pageSize() noexcept {
#ifdef _WIN32
      static const size_t size = [] {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        return static_cast<size_t>(info.dwPageSize);
      }();
#else
      static const auto size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
      return size;
    }
  } // namespace

  void *mapRingMemory(size_t bytes) {
#ifdef _WIN32
    void *memory = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (memory == nullptr) {
      throw std::bad_alloc();
    }
#else
    void *memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
      throw std::bad_alloc();
    }
#endif
    return memory;
  }

  void unmapRingMemory(void *memory, size_t bytes) noexcept {
#ifdef _WIN32
    (void) bytes;
    VirtualFree(memory, 0, MEM_RELEASE);
#else
    munmap(memory, bytes);
#endif
  }

  void discardRingMemory(void *memory, size_t bytes) noexcept {
    const size_t page = pageSize();
    const auto begin = reinterpret_cast<uintptr_t>(memory);
    const uintptr_t first = (begin + page - 1) & ~(page - 1);
    const uintptr_t last = (begin + bytes) & ~(page - 1);
    if (first >= last) {
      return;
    }

    void *const address = static_cast<char *>(memory) + (first - begin);
    const size_t length = last - first;
#ifdef _WIN32
    DiscardVirtualMemory(address, length);
#else
    madvise(address, length, MADV_DONTNEED);
#endif
  }
} // namespace sc::threading::internal
