#include "sc/memory/virtual_memory.hpp"

#include <new>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace sc::memory {

  size_t pageSize() noexcept {
#ifdef _WIN32
    static const size_t size = [] {
      SYSTEM_INFO info;
      GetSystemInfo(&info);
      return static_cast<size_t>(info.dwPageSize);
    }();
#else
    static const auto size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
    return size;
  }

  void *reserve(size_t bytes) {
#ifdef _WIN32
    void *address = VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE);
    if (address == nullptr) {
      throw std::bad_alloc{};
    }
#else
    // Not counted against the memory there is: most of it is never touched.
    void *address = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (address == MAP_FAILED) {
      throw std::bad_alloc{};
    }
#endif
    return address;
  }

  void preferHugePages([[maybe_unused]] void *address, [[maybe_unused]] size_t bytes) noexcept {
#ifdef __linux__
    madvise(address, bytes, MADV_HUGEPAGE);
#endif
  }

  void preferSmallPages([[maybe_unused]] void *address, [[maybe_unused]] size_t bytes) noexcept {
#ifdef __linux__
    madvise(address, bytes, MADV_NOHUGEPAGE);
#endif
  }

  void commit([[maybe_unused]] void *address, [[maybe_unused]] size_t bytes) {
#ifdef _WIN32
    if (VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
      throw std::bad_alloc{};
    }
#endif
    // Elsewhere the memory comes with the first access.
  }

  void populate([[maybe_unused]] void *address, [[maybe_unused]] size_t bytes) noexcept {
#if defined(__linux__) && defined(MADV_POPULATE_WRITE)
    const Pieces pieces = piecesAround(reinterpret_cast<uintptr_t>(address), bytes, pageSize());
    madvise(startOf(address, pieces), pieces.bytes, MADV_POPULATE_WRITE);
#endif
  }

  void collapse([[maybe_unused]] void *address, [[maybe_unused]] size_t bytes) noexcept {
#if defined(__linux__) && defined(MADV_COLLAPSE)
    madvise(address, bytes, MADV_COLLAPSE);
#endif
  }

  void discard(void *address, size_t bytes) noexcept {
    const Pieces pieces = piecesWithin(reinterpret_cast<uintptr_t>(address), bytes, pageSize());
    if (pieces.bytes == 0) {
      return;
    }
#ifdef _WIN32
    DiscardVirtualMemory(startOf(address, pieces), pieces.bytes);
#else
    madvise(startOf(address, pieces), pieces.bytes, MADV_DONTNEED);
#endif
  }

  void offer(void *address, size_t bytes) noexcept {
    const Pieces pieces = piecesWithin(reinterpret_cast<uintptr_t>(address), bytes, pageSize());
    if (pieces.bytes == 0) {
      return;
    }
#ifdef _WIN32
    // The memory just stays. Windows has no MADV_FREE: DiscardVirtualMemory
    // throws the pages away at once, and so does Wine with MEM_RESET, which
    // is meant to leave them until they are needed. Either took 11.8 instead
    // of 1.35 ms to create 100 000 entities with 320 bytes of components each
    // in the columns of a world before, as without memory::rangeCache.
#elifdef MADV_FREE
    // Before Linux 4.5 there is no MADV_FREE, and the memory just stays.
    madvise(startOf(address, pieces), pieces.bytes, MADV_FREE);
#else
    madvise(startOf(address, pieces), pieces.bytes, MADV_DONTNEED);
#endif
  }

  void decommit(void *address, size_t bytes) noexcept {
    const Pieces pieces = piecesWithin(reinterpret_cast<uintptr_t>(address), bytes, pageSize());
    if (pieces.bytes == 0) {
      return;
    }
#ifdef _WIN32
    VirtualFree(startOf(address, pieces), pieces.bytes, MEM_DECOMMIT);
#elifdef MADV_FREE
    // Linux promises nothing for a range reserved with MAP_NORESERVE, so
    // there is nothing to take back but the memory, and that only when it is
    // needed elsewhere, as with offer: taken back at once, a column that was
    // emptied and filled again fetched every piece of it anew, 528 instead of
    // 220 µs to refill 100 000 entities with two components, and emptying it
    // took 855 instead of 765 µs.
    madvise(startOf(address, pieces), pieces.bytes, MADV_FREE);
#else
    // Before Linux 4.5 there is no MADV_FREE.
    madvise(startOf(address, pieces), pieces.bytes, MADV_DONTNEED);
#endif
  }

  void release(void *address, [[maybe_unused]] size_t bytes) noexcept {
#ifdef _WIN32
    VirtualFree(address, 0, MEM_RELEASE);
#else
    munmap(address, bytes);
#endif
  }

} // namespace sc::memory
