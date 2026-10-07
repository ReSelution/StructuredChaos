#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <type_traits>

#include "sc/util/fixed_string.hpp"

namespace {

  using sc::FixedString;

  // Mirrors how Logger and Stat take their names.
  template <FixedString Name> struct Named {
    static constexpr std::string_view name = Name;
  };

  template <FixedString A, FixedString B = ""> struct Pair {
    static constexpr std::string_view first = A;
    static constexpr std::string_view second = B;
  };

  // Everything here is usable in constant expressions.
  constexpr FixedString HELLO = "Hello";
  static_assert(sizeof(HELLO) == 6);
  static_assert(HELLO.text() == "Hello");
  static_assert(std::string_view{HELLO} == "Hello");
  static_assert(HELLO.text().size() == 5);
  static_assert(HELLO.buf[5] == '\0');

  static_assert(std::is_same_v<decltype(FixedString{"abc"}), FixedString<4>>);
  static_assert(std::is_same_v<decltype(FixedString{""}), FixedString<1>>);

  static_assert(std::is_same_v<Named<"Chaos">, Named<"Chaos">>);
  static_assert(!std::is_same_v<Named<"Chaos">, Named<"chaos">>);
  static_assert(!std::is_same_v<Named<"Chaos">, Named<"Chao">>);
  static_assert(!std::is_same_v<Named<"ab">, Named<"ba">>);

} // namespace

TEST_CASE("FixedString Stores The Literal", "[util][fixed_string]") {
  SECTION("Text Excludes The Terminator") {
    constexpr FixedString s = "Registry";

    REQUIRE(s.text() == "Registry");
    REQUIRE(s.text().size() == 8);
    REQUIRE(sizeof(s) == 9);
    REQUIRE(s.buf[8] == '\0');
  }

  SECTION("Converts Implicitly To string_view") {
    constexpr FixedString s = "Threading";
    std::string_view view = s;

    REQUIRE(view == "Threading");
    REQUIRE(view.data() == s.buf);
  }

  SECTION("Empty Literal") {
    constexpr FixedString s = "";

    REQUIRE(s.text().empty());
    REQUIRE(sizeof(s) == 1);
    REQUIRE(std::string_view{s}.empty());
  }

  SECTION("Keeps Spaces And Punctuation") {
    constexpr FixedString s = "Batch Processing: 100%";

    REQUIRE(s.text() == "Batch Processing: 100%");
  }

  SECTION("Keeps Embedded Terminators") {
    constexpr FixedString s = "a\0b";

    REQUIRE(s.text().size() == 3);
    REQUIRE(s.text() == std::string_view("a\0b", 3));
  }

  SECTION("Owns A Copy Of The Characters") {
    char source[] = "copy";
    FixedString<5> s{source};
    source[0] = 'X';

    REQUIRE(s.text() == "copy");
  }
}

TEST_CASE("FixedString As Template Parameter", "[util][fixed_string]") {
  SECTION("Name Is Available At Compile Time") {
    REQUIRE(Named<"Chaos">::name == "Chaos");
    REQUIRE(Named<"">::name.empty());
  }

  SECTION("Equal Text Yields The Same Type") {
    REQUIRE(std::is_same_v<Named<"Mem">, Named<"Mem">>);
    REQUIRE(Named<"Mem">::name.data() == Named<"Mem">::name.data());
  }

  SECTION("Different Text Yields Different Types") {
    REQUIRE_FALSE(std::is_same_v<Named<"Mem">, Named<"mem">>);
    REQUIRE_FALSE(std::is_same_v<Named<"Mem">, Named<"Memo">>);
  }

  SECTION("Works As Defaulted Second Parameter") {
    REQUIRE(Pair<"Module">::first == "Module");
    REQUIRE(Pair<"Module">::second.empty());
    REQUIRE(Pair<"Module", "Category">::second == "Category");
    REQUIRE_FALSE(std::is_same_v<Pair<"Module">, Pair<"Module", "Category">>);
  }

  SECTION("Text Can Be Copied Into A std::string") {
    std::string copy{Named<"Stats">::name};

    REQUIRE(copy == "Stats");
  }
}
