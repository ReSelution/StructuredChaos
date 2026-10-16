#pragma once

#include <cstddef>
#include <cstdint>
#include <new>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

// A range of addresses that gets its memory piece by piece: reserved once in
// full, which costs nothing, and backed with memory only where it is used.
namespace sc::memory {

  // The size of the pieces the OS hands out and takes back, at least: what
  // the parts of a range are laid out in. The pieces of the OS at hand are of
  // this size or a multiple of it, see pageSize.
  inline constexpr size_t OS_PAGE_SIZE = 4096;
  // What the OS may back a range with instead, see reserve. Where its huge
  // pages are of another size, a part of this size is still what
  // preferHugePages and collapse are asked for, and they do what they can.
  inline constexpr size_t HUGE_PAGE_SIZE = size_t{1} << 21;

  // The size of the pieces the OS at hand hands out and takes back: 4096
  // bytes mostly, 16384 or 65536 on some.
  [[nodiscard]] inline size_t pageSize() noexcept {
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

  // A row of whole pieces, by the address it starts at.
  struct Pieces {
    uintptr_t address;
    size_t bytes;
  };

  // The pieces of the given size, a power of two, that lie in the part as a
  // whole: none of their bytes is outside of it.
  [[nodiscard]] constexpr Pieces piecesWithin(uintptr_t address, size_t bytes, size_t piece) noexcept {
    const uintptr_t first = (address + piece - 1) & ~(uintptr_t{piece} - 1);
    const uintptr_t end = (address + bytes) & ~(uintptr_t{piece} - 1);
    return {.address = first, .bytes = end > first ? end - first : 0};
  }

  // The pieces that have a byte of the part.
  [[nodiscard]] constexpr Pieces piecesAround(uintptr_t address, size_t bytes, size_t piece) noexcept {
    const uintptr_t first = address & ~(uintptr_t{piece} - 1);
    const uintptr_t end = (address + bytes + piece - 1) & ~(uintptr_t{piece} - 1);
    return {.address = first, .bytes = end - first};
  }

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

  // Gives a part of a reserved range its memory now and in one go, instead of
  // piece by piece with the first access to each piece. What is in the part
  // stays. Does nothing where the OS has no means for it; the memory then
  // comes with the accesses as usual. Every piece of the OS that has a byte
  // of the part gets its memory.
  inline void populate([[maybe_unused]] void *address, [[maybe_unused]] size_t bytes) noexcept {
#if defined(__linux__) && defined(MADV_POPULATE_WRITE)
    const Pieces pieces = piecesAround(reinterpret_cast<uintptr_t>(address), bytes, pageSize());
    madvise(reinterpret_cast<void *>(pieces.address), pieces.bytes, MADV_POPULATE_WRITE);
#endif
  }

  // Moves what a part of a reserved range has in small pages into huge ones,
  // right away. Both ends must be multiples of HUGE_PAGE_SIZE. A huge page is
  // backed in full afterwards, also where nothing was before. Does nothing
  // where the OS has no means for it or no huge page at hand.
  inline void collapse([[maybe_unused]] void *address, [[maybe_unused]] size_t bytes) noexcept {
#if defined(__linux__) && defined(MADV_COLLAPSE)
    madvise(address, bytes, MADV_COLLAPSE);
#endif
  }

  // Hands the memory of a part back to the OS. The part stays usable; what is
  // in it afterwards is not defined, what is next to it stays. Only the
  // pieces of the OS that lie in the part as a whole go back: all of it if
  // both ends are multiples of pageSize, less otherwise and possibly nothing.
  inline void discard(void *address, size_t bytes) noexcept {
    const Pieces pieces = piecesWithin(reinterpret_cast<uintptr_t>(address), bytes, pageSize());
    if (pieces.bytes == 0) {
      return;
    }
#ifdef _WIN32
    DiscardVirtualMemory(reinterpret_cast<void *>(pieces.address), pieces.bytes);
#else
    madvise(reinterpret_cast<void *>(pieces.address), pieces.bytes, MADV_DONTNEED);
#endif
  }

  // Lets the OS take the memory of a part whenever it needs it, and leaves it
  // where it is until then. The part stays usable: a piece the OS did not
  // take still has what was in it and costs nothing to use again, one it took
  // comes back as after discard. Where the OS cannot do it later, it does it
  // now, as discard. Only the pieces of the OS that lie in the part as a whole
  // are offered, as with discard. The memory still counts as the process'
  // until it is taken: Rss on Linux, with LazyFree telling how much of it
  // may go.
  inline void offer(void *address, size_t bytes) noexcept {
    const Pieces pieces = piecesWithin(reinterpret_cast<uintptr_t>(address), bytes, pageSize());
    if (pieces.bytes == 0) {
      return;
    }
#ifdef _WIN32
    DiscardVirtualMemory(reinterpret_cast<void *>(pieces.address), pieces.bytes);
#elif defined(MADV_FREE)
    // Before Linux 4.5 there is no MADV_FREE, and the memory just stays.
    madvise(reinterpret_cast<void *>(pieces.address), pieces.bytes, MADV_FREE);
#else
    madvise(reinterpret_cast<void *>(pieces.address), pieces.bytes, MADV_DONTNEED);
#endif
  }

  // Takes the memory of a part back from the OS and with it the promise of
  // memory for it, which on Windows counts against the commit limit for as
  // long as the part is committed. On Windows the part is not usable after
  // this until commit is called for it again; elsewhere it stays usable, as
  // after discard. What is in it once it is used again is not defined. Only
  // the pieces of the OS that lie in the part as a whole are taken back, as
  // with discard.
  inline void decommit(void *address, size_t bytes) noexcept {
    const Pieces pieces = piecesWithin(reinterpret_cast<uintptr_t>(address), bytes, pageSize());
    if (pieces.bytes == 0) {
      return;
    }
#ifdef _WIN32
    VirtualFree(reinterpret_cast<void *>(pieces.address), pieces.bytes, MEM_DECOMMIT);
#else
    // Linux promises nothing for a range reserved with MAP_NORESERVE, so
    // there is nothing to take back but the memory.
    madvise(reinterpret_cast<void *>(pieces.address), pieces.bytes, MADV_DONTNEED);
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
