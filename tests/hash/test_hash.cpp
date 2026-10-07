#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sc/hash/hash.hpp"

namespace {

constexpr size_t MAX_LENGTH = 300;

// Mixed case, digits and punctuation around the letters in the ASCII table,
// repeated up to MAX_LENGTH characters.
constexpr std::array<char, MAX_LENGTH> make_text() {
  constexpr std::string_view pattern = "Hello, World! @[`{ AZaz 0123456789 /Game/FactoryGame/Buildable_C ";
  std::array<char, MAX_LENGTH> text{};
  for (size_t i = 0; i < MAX_LENGTH; ++i) {
    text[i] = pattern[i % pattern.size()];
  }
  return text;
}

constexpr std::array<char, MAX_LENGTH> TEXT = make_text();

constexpr char lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

constexpr std::array<char, MAX_LENGTH> make_lower_text() {
  std::array<char, MAX_LENGTH> text = TEXT;
  for (char &c : text) {
    c = lower(c);
  }
  return text;
}

constexpr std::array<char, MAX_LENGTH> LOWER_TEXT = make_lower_text();

// The hash of every prefix of a text, computed while compiling.
template <typename Hash>
constexpr std::array<sc::h64, MAX_LENGTH + 1>
hash_prefixes(const std::array<char, MAX_LENGTH> &text, Hash hash) {
  std::array<sc::h64, MAX_LENGTH + 1> result{};
  for (size_t length = 0; length <= MAX_LENGTH; ++length) {
    result[length] = hash(std::string_view{text.data(), length});
  }
  return result;
}

constexpr auto COMPILED_HASHES =
    hash_prefixes(TEXT, [](std::string_view s) { return sc::hash(s); });
constexpr auto COMPILED_LOWER_HASHES = hash_prefixes(
    TEXT, [](std::string_view s) { return sc::hash_lowercase(s); });

enum class TypedHash : sc::h64 {};

} // namespace

TEST_CASE("Hash Is The Same At Compile Time And At Run Time", "[hash]") {
  SECTION("Strings Of Every Length") {
    // Copied at run time, so the calls below cannot be constant evaluated.
    const std::string text(TEXT.data(), TEXT.size());

    size_t first_difference = MAX_LENGTH + 1;
    for (size_t length = 0; length <= MAX_LENGTH; ++length) {
      if (sc::hash(std::string_view{text.data(), length}) !=
          COMPILED_HASHES[length]) {
        first_difference = length;
        break;
      }
    }
    REQUIRE(first_difference == MAX_LENGTH + 1);
  }

  SECTION("Integers") {
    constexpr sc::h64 compiled8 = sc::hash(uint8_t{0xA5});
    constexpr sc::h64 compiled16 = sc::hash(uint16_t{0xBEEF});
    constexpr sc::h64 compiled32 = sc::hash(uint32_t{0xDEADBEEF});
    constexpr sc::h64 compiled64 = sc::hash(uint64_t{0x1122334455667788});
    constexpr sc::h64 compiledSigned = sc::hash(int32_t{-42});

    uint8_t v8 = 0xA5;
    uint16_t v16 = 0xBEEF;
    uint32_t v32 = 0xDEADBEEF;
    uint64_t v64 = 0x1122334455667788;
    int32_t vSigned = -42;

    REQUIRE(sc::hash(v8) == compiled8);
    REQUIRE(sc::hash(v16) == compiled16);
    REQUIRE(sc::hash(v32) == compiled32);
    REQUIRE(sc::hash(v64) == compiled64);
    REQUIRE(sc::hash(vSigned) == compiledSigned);

    // The smallest integer types, which need no byte order at all.
    constexpr sc::h64 compiledBool = sc::hash(true);
    constexpr sc::h64 compiledChar = sc::hash('x');
    bool vBool = true;
    char vChar = 'x';
    REQUIRE(sc::hash(vBool) == compiledBool);
    REQUIRE(sc::hash(vChar) == compiledChar);
    REQUIRE(sc::hash(vBool) == sc::hash(uint8_t{1}));
  }

  SECTION("Pairs Of Integers") {
    constexpr sc::h64 compiled = sc::hash(uint64_t{1}, uint64_t{2});
    constexpr sc::h64 compiledMixed = sc::hash(uint32_t{7}, uint64_t{9});

    uint64_t a = 1, b = 2;
    uint32_t c = 7;
    uint64_t d = 9;

    REQUIRE(sc::hash(a, b) == compiled);
    REQUIRE(sc::hash(c, d) == compiledMixed);
    REQUIRE(sc::hash(a, b) != sc::hash(b, a));
  }
}

TEST_CASE("Hash Treats All Ways To Pass A String Alike", "[hash]") {
  constexpr sc::h64 expected = sc::hash(std::string_view{"Buildable_C"});

  const std::string owned = "Buildable_C";
  const char *c_string = owned.c_str();
  const std::vector<char> characters(owned.begin(), owned.end());
  const std::vector<uint8_t> bytes(owned.begin(), owned.end());

  REQUIRE(sc::hash(owned) == expected);
  REQUIRE(sc::hash(std::string_view{owned}) == expected);
  REQUIRE(sc::hash(owned.data(), owned.size()) == expected);
  REQUIRE(sc::hash(characters) == expected);
  REQUIRE(sc::hash(bytes) == expected);
  REQUIRE("Buildable_C"_h == expected);

  // A literal or a C string passed directly, without a length.
  REQUIRE(sc::hash("Buildable_C") == expected);
  REQUIRE(sc::hash(c_string) == expected);
  STATIC_REQUIRE(sc::hash("Buildable_C") == sc::hash(std::string_view{"Buildable_C"}));
}

TEST_CASE("Hash Basics", "[hash]") {
  SECTION("Different Inputs Give Different Hashes") {
    REQUIRE(sc::hash(std::string_view{"a"}) != sc::hash(std::string_view{"b"}));
    REQUIRE(sc::hash(std::string_view{"ab"}) != sc::hash(std::string_view{"ba"}));
    REQUIRE(sc::hash(std::string_view{""}) != sc::hash(std::string_view{"a"}));
    REQUIRE(sc::hash(uint64_t{0}) != sc::hash(uint64_t{1}));
    // The same number in another width is another sequence of bytes.
    REQUIRE(sc::hash(uint32_t{1}) != sc::hash(uint64_t{1}));
  }

  SECTION("No Collisions Among Many Small Numbers") {
    constexpr uint64_t count = 200'000;
    std::unordered_set<sc::h64> seen;
    seen.reserve(count);
    for (uint64_t i = 0; i < count; ++i) {
      seen.insert(sc::hash(i));
    }
    REQUIRE(seen.size() == count);
  }

  SECTION("Typed Hash") {
    constexpr TypedHash typed = sc::hash<TypedHash>(std::string_view{"abc"});
    REQUIRE(static_cast<sc::h64>(typed) == sc::hash(std::string_view{"abc"}));
  }
}

TEST_CASE("Lowercase Hash", "[hash]") {
  const std::string text(TEXT.data(), TEXT.size());
  const std::string lower_text(LOWER_TEXT.data(), LOWER_TEXT.size());

  SECTION("Equals The Hash Of The Lowercased String, For Every Length") {
    size_t first_difference = MAX_LENGTH + 1;
    for (size_t length = 0; length <= MAX_LENGTH; ++length) {
      if (sc::hash_lowercase(std::string_view{text.data(), length}) !=
          sc::hash(std::string_view{lower_text.data(), length})) {
        first_difference = length;
        break;
      }
    }
    REQUIRE(first_difference == MAX_LENGTH + 1);
  }

  SECTION("Is The Same At Compile Time And At Run Time") {
    size_t first_difference = MAX_LENGTH + 1;
    for (size_t length = 0; length <= MAX_LENGTH; ++length) {
      if (sc::hash_lowercase(std::string_view{text.data(), length}) !=
          COMPILED_LOWER_HASHES[length]) {
        first_difference = length;
        break;
      }
    }
    REQUIRE(first_difference == MAX_LENGTH + 1);
  }

  SECTION("Long Strings At Compile Time") {
    static constexpr auto LONG_TEXT = [] {
      std::array<char, 2000> long_text{};
      for (size_t i = 0; i < long_text.size(); ++i) {
        long_text[i] = static_cast<char>('A' + i % 26);
      }
      return long_text;
    }();
    constexpr sc::h64 compiled =
        sc::hash_lowercase(std::string_view{LONG_TEXT.data(), LONG_TEXT.size()});

    const std::string long_text(LONG_TEXT.data(), LONG_TEXT.size());
    REQUIRE(sc::hash_lowercase(std::string_view{long_text}) == compiled);
  }

  SECTION("Only Touches The Letters A To Z") {
    // Every single byte, and every byte as the last one of a short string,
    // where it is read on a path of its own.
    int differences = 0;
    for (int value = 0; value < 256; ++value) {
      const char c = static_cast<char>(value);
      for (const std::string &sample :
           {std::string(1, c), std::string("x") + c, std::string("xy") + c,
            std::string("0123456") + c, std::string(40, 'q') + c}) {
        std::string expected = sample;
        for (char &e : expected) {
          e = lower(e);
        }
        if (sc::hash_lowercase(std::string_view{sample}) !=
            sc::hash(std::string_view{expected})) {
          ++differences;
        }
      }
    }
    REQUIRE(differences == 0);
  }
}

TEST_CASE("IdentityHash", "[hash]") {
  SECTION("Passes Numbers Through And Hashes Strings") {
    sc::IdentityHash hasher;
    REQUIRE(hasher(uint64_t{0x1234}) == 0x1234);
    REQUIRE(hasher(std::string_view{"abc"}) == sc::hash(std::string_view{"abc"}));
    REQUIRE(hasher(std::string{"abc"}) == hasher(std::string_view{"abc"}));
  }

  SECTION("Looks Up A String Key By View") {
    std::unordered_map<std::string, int, sc::IdentityHash, std::equal_to<>> map;
    map.emplace("Desc_IronPlate_C", 7);

    const std::string_view key = "Desc_IronPlate_C";
    const auto found = map.find(key);
    REQUIRE(found != map.end());
    REQUIRE(found->second == 7);
  }
}
