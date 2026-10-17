#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sc/memory/virtual_memory.hpp"

#ifdef _WIN32
// For VirtualQuery, which the header leaves out on purpose.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

  using sc::memory::Pieces;
  using sc::memory::piecesAround;
  using sc::memory::piecesWithin;

  constexpr bool same(Pieces pieces, uintptr_t address, size_t bytes) {
    return pieces.address == address && pieces.bytes == bytes;
  }

} // namespace

TEST_CASE("Virtual Memory", "[memory][virtual]") {
  SECTION("The Pieces Within A Part And Around It") {
    // Both ends at a piece: the part itself.
    STATIC_REQUIRE(same(piecesWithin(0x10000, 0x8000, 0x1000), 0x10000, 0x8000));
    STATIC_REQUIRE(same(piecesAround(0x10000, 0x8000, 0x1000), 0x10000, 0x8000));
    // A part of 4096 bytes where the pieces are larger, as the values of a
    // page of a column of bytes are: none of them is its own.
    STATIC_REQUIRE(piecesWithin(0x10000, 0x1000, 0x4000).bytes == 0);
    STATIC_REQUIRE(piecesWithin(0x11000, 0x1000, 0x4000).bytes == 0);
    STATIC_REQUIRE(piecesWithin(0x13000, 0x2000, 0x4000).bytes == 0);
    STATIC_REQUIRE(same(piecesAround(0x11000, 0x1000, 0x4000), 0x10000, 0x4000));
    STATIC_REQUIRE(same(piecesAround(0x13000, 0x2000, 0x4000), 0x10000, 0x8000));
    // Of 3 * 4096 bytes: one in four has a piece to itself.
    STATIC_REQUIRE(same(piecesWithin(0x10000, 0x3000, 0x4000), 0x10000, 0));
    STATIC_REQUIRE(same(piecesWithin(0x13000, 0x3000, 0x4000), 0x14000, 0));
    STATIC_REQUIRE(same(piecesWithin(0x1f000, 0x6000, 0x4000), 0x20000, 0x4000));
    STATIC_REQUIRE(same(piecesWithin(0x1e000, 0xb000, 0x4000), 0x20000, 0x8000));
    STATIC_REQUIRE(same(piecesAround(0x1e000, 0xb000, 0x4000), 0x1c000, 0x10000));
  }

  SECTION("The Pieces Of This OS") {
    const size_t page = sc::memory::pageSize();
    REQUIRE(page >= sc::memory::OS_PAGE_SIZE);
    REQUIRE(page % sc::memory::OS_PAGE_SIZE == 0);
    REQUIRE((page & (page - 1)) == 0);
  }

  SECTION("Discarding Leaves What Is Next To The Part") {
    const size_t page = sc::memory::pageSize();
    const size_t bytes = 4 * page;
    auto *const start = static_cast<unsigned char *>(sc::memory::reserve(bytes));
    sc::memory::commit(start, bytes);
    sc::memory::populate(start + 10, page);
    std::memset(start, 0xab, bytes);

    // From the middle of the first piece to the middle of the last one.
    sc::memory::discard(start + (page / 2), 3 * page);
    const auto kept = [](const unsigned char *first, const unsigned char *last) {
      return std::all_of(first, last, [](unsigned char byte) { return byte == 0xab; });
    };
    REQUIRE(kept(start, start + page));
    REQUIRE(kept(start + (3 * page), start + bytes));
#ifdef __linux__
    // The two in between went back and come back empty.
    REQUIRE(std::all_of(start + page, start + (3 * page), [](unsigned char byte) { return byte == 0; }));
#endif

    // Less than a piece, and not at one: nothing goes back.
    std::memset(start, 0xab, bytes);
    sc::memory::discard(start + (page / 4), page / 2);
    sc::memory::discard(start + (page / 2), page);
    REQUIRE(kept(start, start + bytes));

    // Still usable.
    std::memset(start, 0xcd, bytes);
    REQUIRE(start[bytes - 1] == 0xcd);
    sc::memory::release(start, bytes);
  }

  SECTION("Decommitted Memory Is Usable Again After Commit") {
    const size_t page = sc::memory::pageSize();
    const size_t bytes = 4 * page;
    auto *const start = static_cast<unsigned char *>(sc::memory::reserve(bytes));
    sc::memory::commit(start, bytes);
    std::memset(start, 0xab, bytes);

    sc::memory::decommit(start + page, 2 * page);
#ifdef _WIN32
    // No longer committed, and only the part itself.
    MEMORY_BASIC_INFORMATION info{};
    REQUIRE(VirtualQuery(start + page, &info, sizeof(info)) != 0);
    REQUIRE(info.State == MEM_RESERVE);
    REQUIRE(info.RegionSize == 2 * page);
    REQUIRE(VirtualQuery(start, &info, sizeof(info)) != 0);
    REQUIRE(info.State == MEM_COMMIT);
#endif
    const auto all = [](const unsigned char *first, const unsigned char *last, unsigned char value) {
      return std::all_of(first, last, [value](unsigned char byte) { return byte == value; });
    };
    REQUIRE(all(start, start + page, 0xab));
    REQUIRE(all(start + (3 * page), start + bytes, 0xab));

    sc::memory::commit(start + page, 2 * page);
    std::memset(start, 0xcd, bytes);
    REQUIRE(all(start, start + bytes, 0xcd));
    REQUIRE(start[bytes - 1] == 0xcd);
    sc::memory::release(start, bytes);
  }

  SECTION("Offered Memory Stays Usable And Leaves What Is Next To It") {
    const size_t page = sc::memory::pageSize();
    const size_t bytes = 4 * page;
    auto *const start = static_cast<unsigned char *>(sc::memory::reserve(bytes));
    sc::memory::commit(start, bytes);
    std::memset(start, 0xab, bytes);

    // From the middle of the first piece to the middle of the last one.
    sc::memory::offer(start + (page / 2), 3 * page);
    const auto all = [](const unsigned char *first, const unsigned char *last, unsigned char value) {
      return std::all_of(first, last, [value](unsigned char byte) { return byte == value; });
    };
    REQUIRE(all(start, start + page, 0xab));
    REQUIRE(all(start + (3 * page), start + bytes, 0xab));
    // The two in between are as they were, unless the OS took them.
    for (size_t piece = 1; piece < 3; ++piece) {
      const unsigned char *const first = start + (piece * page);
      REQUIRE((all(first, first + page, 0xab) || all(first, first + page, 0)));
    }

    // Usable without a commit.
    std::memset(start, 0xcd, bytes);
    REQUIRE(all(start, start + bytes, 0xcd));
    sc::memory::release(start, bytes);
  }
}
