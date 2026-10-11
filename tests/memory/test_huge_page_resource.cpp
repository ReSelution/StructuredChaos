#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <vector>

#include "sc/memory/huge_page_resource.hpp"
#include "sc/memory/monotonic_resource.hpp"

namespace {

  constexpr size_t HugePage = sc::HugePageResource::HUGE_PAGE_SIZE;

  bool isAligned(const void *ptr, size_t alignment) { return reinterpret_cast<uintptr_t>(ptr) % alignment == 0; }

} // namespace

TEST_CASE("HugePageResource: a large block is usable from start to end", "[memory][hugepage]") {
  auto *res = sc::HugePageResource::get();

  // Not a multiple of the huge page size on purpose.
  const size_t size = (3 * HugePage) + 4096;
  auto *block = static_cast<std::byte *>(res->allocate(size, alignof(std::max_align_t)));
  REQUIRE(block != nullptr);
#ifdef __linux__
  CHECK(isAligned(block, HugePage));
#endif

  std::memset(block, 0x5A, size);
  CHECK(block[0] == std::byte{0x5A});
  CHECK(block[size - 1] == std::byte{0x5A});

  res->deallocate(block, size, alignof(std::max_align_t));
}

TEST_CASE("HugePageResource: blocks do not overlap", "[memory][hugepage]") {
  auto *res = sc::HugePageResource::get();

  auto *first = static_cast<std::byte *>(res->allocate(HugePage, 8));
  auto *second = static_cast<std::byte *>(res->allocate(HugePage, 8));
  REQUIRE(first != second);

  std::memset(first, 1, HugePage);
  std::memset(second, 2, HugePage);
  CHECK(first[HugePage - 1] == std::byte{1});
  CHECK(second[0] == std::byte{2});

  res->deallocate(first, HugePage, 8);
  res->deallocate(second, HugePage, 8);
}

TEST_CASE("HugePageResource: small and over-aligned requests work as well", "[memory][hugepage]") {
  auto *res = sc::HugePageResource::get();

  SECTION("below the huge page size") {
    auto *block = static_cast<std::byte *>(res->allocate(100, 64));
    REQUIRE(block != nullptr);
    CHECK(isAligned(block, 64));
    std::memset(block, 7, 100);
    res->deallocate(block, 100, 64);
  }

  SECTION("aligned stricter than a huge page") {
    const size_t alignment = 2 * HugePage;
    auto *block = static_cast<std::byte *>(res->allocate(alignment, alignment));
    REQUIRE(block != nullptr);
    CHECK(isAligned(block, alignment));
    std::memset(block, 7, alignment);
    res->deallocate(block, alignment, alignment);
  }
}

TEST_CASE("HugePageResource: there is one instance, equal only to itself", "[memory][hugepage]") {
  auto *res = sc::HugePageResource::get();

  CHECK(res == sc::HugePageResource::get());
  CHECK(res->is_equal(*res));
  CHECK_FALSE(res->is_equal(*std::pmr::new_delete_resource()));
}

TEST_CASE("HugePageResource: works as an upstream", "[memory][hugepage]") {
  SECTION("of a MonotonicResource") {
    sc::MonotonicResource res(4 * HugePage, sc::HugePageResource::get());
    auto span = res.allocateSpan<uint64_t>(HugePage / sizeof(uint64_t));
    span.front() = 1;
    span.back() = 2;
    CHECK(span.front() + span.back() == 3);
  }

  SECTION("of a pmr container") {
    std::pmr::vector<uint64_t> values(sc::HugePageResource::get());
    for (uint64_t i = 0; i < 600'000; ++i) {
      values.push_back(i);
    }
    CHECK(values.back() == 599'999);
  }
}
