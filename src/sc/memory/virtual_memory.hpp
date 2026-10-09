#pragma once

#include <cstddef>
#include <new>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

// A range of addresses that gets its memory piece by piece: reserved once in
// full, which costs nothing, and backed with memory only where it is used.
namespace sc::memory {

  // The size of the pieces the OS hands out and takes back.
  inline constexpr size_t OS_PAGE_SIZE = 4096;
  // What the OS may back a range with instead, see reserve.
  inline constexpr size_t HUGE_PAGE_SIZE = size_t{1} << 21;

  // Addresses for the given number of bytes, without memory behind them yet.
  // Throws std::bad_alloc if the OS has no room for them.
  [[nodiscard]] inline void *reserve(size_t bytes) {
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

  // Lets the OS back the part of a reserved range with huge pages. Fewer,
  // larger pieces: faster to go through, but a single byte in use holds a
  // whole huge page.
  inline void preferHugePages([[maybe_unused]] void *address, [[maybe_unused]] size_t bytes) noexcept {
#ifdef __linux__
    madvise(address, bytes, MADV_HUGEPAGE);
#endif
  }

  // The opposite: only the pieces that are used get memory, each of them
  // OS_PAGE_SIZE large.
  inline void preferSmallPages([[maybe_unused]] void *address, [[maybe_unused]] size_t bytes) noexcept {
#ifdef __linux__
    madvise(address, bytes, MADV_NOHUGEPAGE);
#endif
  }

  // Makes a part of a reserved range usable; its bytes are zero the first
  // time. Safe to call again for a part that is in use already, and from
  // several threads. Throws std::bad_alloc if there is no memory left.
  inline void commit([[maybe_unused]] void *address, [[maybe_unused]] size_t bytes) {
#ifdef _WIN32
    if (VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
      throw std::bad_alloc{};
    }
#endif
    // Elsewhere the memory comes with the first access.
  }

  // Hands the memory of a part back to the OS. The part stays usable; what is
  // in it afterwards is not defined. Both ends must be multiples of
  // OS_PAGE_SIZE.
  inline void discard(void *address, size_t bytes) noexcept {
#ifdef _WIN32
    DiscardVirtualMemory(address, bytes);
#else
    madvise(address, bytes, MADV_DONTNEED);
#endif
  }

  // Gives the whole range back, with the address and size it was reserved
  // with.
  inline void release(void *address, [[maybe_unused]] size_t bytes) noexcept {
#ifdef _WIN32
    VirtualFree(address, 0, MEM_RELEASE);
#else
    munmap(address, bytes);
#endif
  }

} // namespace sc::memory
