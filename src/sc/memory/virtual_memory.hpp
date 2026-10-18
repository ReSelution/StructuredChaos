#pragma once

#include <cstddef>
#include <cstdint>

// The calls to the OS are in virtual_memory.cpp: <windows.h> defines macros
// such as min, max, near, far and small, which would take the place of
// whatever has those names wherever this header is included.

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
  [[nodiscard]] size_t pageSize() noexcept;

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

  // Where the pieces of a part start, as a pointer into the same range as
  // the part: reached from it instead of made from the number, which would
  // leave the compiler no idea what it points into.
  [[nodiscard]] inline void *startOf(void *part, Pieces pieces) noexcept {
    const auto offset =
        static_cast<ptrdiff_t>(pieces.address) - static_cast<ptrdiff_t>(reinterpret_cast<uintptr_t>(part));
    return static_cast<std::byte *>(part) + offset;
  }

  // Addresses for the given number of bytes, without memory behind them yet.
  // Throws std::bad_alloc if the OS has no room for them.
  [[nodiscard]] void *reserve(size_t bytes);

  // Lets the OS back the part of a reserved range with huge pages. Fewer,
  // larger pieces: faster to go through, but a single byte in use holds a
  // whole huge page.
  void preferHugePages(void *address, size_t bytes) noexcept;

  // The opposite: only the pieces that are used get memory, each of them
  // OS_PAGE_SIZE large.
  void preferSmallPages(void *address, size_t bytes) noexcept;

  // Makes a part of a reserved range usable; its bytes are zero the first
  // time. Safe to call again for a part that is in use already, and from
  // several threads. Throws std::bad_alloc if there is no memory left.
  void commit(void *address, size_t bytes);

  // Gives a part of a reserved range its memory now and in one go, instead of
  // piece by piece with the first access to each piece. What is in the part
  // stays. Does nothing where the OS has no means for it; the memory then
  // comes with the accesses as usual. Every piece of the OS that has a byte
  // of the part gets its memory.
  void populate(void *address, size_t bytes) noexcept;

  // Moves what a part of a reserved range has in small pages into huge ones,
  // right away. Both ends must be multiples of HUGE_PAGE_SIZE. A huge page is
  // backed in full afterwards, also where nothing was before. Does nothing
  // where the OS has no means for it or no huge page at hand.
  void collapse(void *address, size_t bytes) noexcept;

  // Hands the memory of a part back to the OS. The part stays usable; what is
  // in it afterwards is not defined, what is next to it stays. Only the
  // pieces of the OS that lie in the part as a whole go back: all of it if
  // both ends are multiples of pageSize, less otherwise and possibly nothing.
  void discard(void *address, size_t bytes) noexcept;

  // Lets the OS take the memory of a part whenever it needs it, and leaves it
  // where it is until then. The part stays usable: a piece the OS did not
  // take still has what was in it and costs nothing to use again, one it took
  // comes back as after discard. On Windows, which cannot do it later, the
  // memory just stays. Only the pieces of the OS that lie in the part as a
  // whole are offered, as with discard. The memory still counts as the
  // process' until it is taken: Rss on Linux, with LazyFree telling how much
  // of it may go.
  void offer(void *address, size_t bytes) noexcept;

  // Takes the memory of a part back from the OS and with it the promise of
  // memory for it, which on Windows counts against the commit limit for as
  // long as the part is committed. On Windows the part is not usable after
  // this until commit is called for it again; elsewhere it stays usable, and
  // on Linux the memory is offered as with offer, taken only when the OS
  // needs it: until then it counts as the process' (Rss, with LazyFree), and
  // using the part again costs nothing. What is in it once it is used again
  // is not defined. Only the pieces of the OS that lie in the part as a
  // whole are taken back, as with discard.
  void decommit(void *address, size_t bytes) noexcept;

  // Gives the whole range back, with the address and size it was reserved
  // with.
  void release(void *address, size_t bytes) noexcept;

} // namespace sc::memory
