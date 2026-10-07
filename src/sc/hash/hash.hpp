#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "rapidhash-constexpr.h"
#include "rapidhash.h"

#ifdef _MSC_VER
#define SC_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define SC_FORCE_INLINE inline __attribute__((always_inline))
#else
#define SC_FORCE_INLINE inline
#endif

// sc::hash hashes one of:
//   - an integer
//   - two integers
//   - a pointer to bytes and their number
//   - anything with data() and size() over single bytes, such as
//     std::string_view, std::string or std::vector<uint8_t>
//   - a string literal or a C string, up to its terminator
// Every form gives the same result at compile time and at run time.

namespace sc {

  using h64 = uint64_t;

  template <typename T>
  concept SizedBuffer = requires(T a) {
    a.data();
    a.size();
    { a.data() } -> std::same_as<decltype(a.data())>;
    requires std::is_pointer_v<decltype(a.data())>;
    { a.size() } -> std::convertible_to<std::size_t>;
  };

  namespace internal {

    template <typename T>
    concept Byte = sizeof(T) == 1 && (std::integral<T> || std::same_as<T, std::byte>);

    // The one place that picks between the two implementations of rapidhash: the
    // run-time one cannot be evaluated by the compiler.
    template <Byte T> SC_FORCE_INLINE constexpr h64 hashBytes(const T *data, size_t length) noexcept {
      if consteval {
        return rapid::constExpr::rapidhash(data, length);
      } else {
        return ::rapidhash(data, length);
      }
    }

    // Writes an integer in little-endian order and returns its size. Hashing
    // these bytes instead of the integer's memory keeps the result independent
    // of the platform, and lets the compiler do it as well.
    template <std::integral T> SC_FORCE_INLINE constexpr size_t storeBytes(uint8_t *out, T value) noexcept {
      static_assert(sizeof(T) <= sizeof(uint64_t));
      // Widening keeps the low bytes of a negative number as they are in memory.
      const auto bits = static_cast<uint64_t>(value);
      for (size_t i = 0; i < sizeof(T); ++i) {
        out[i] = static_cast<uint8_t>(bits >> (i * 8));
      }
      return sizeof(T);
    }

  } // namespace internal

  template <std::integral T> SC_FORCE_INLINE constexpr h64 hash(T value) noexcept {
    uint8_t bytes[sizeof(T)]{};
    internal::storeBytes(bytes, value);
    return internal::hashBytes(bytes, sizeof(T));
  }

  template <std::integral A, std::integral B> SC_FORCE_INLINE constexpr h64 hash(A first, B second) noexcept {
    uint8_t bytes[sizeof(A) + sizeof(B)]{};
    const size_t offset = internal::storeBytes(bytes, first);
    internal::storeBytes(bytes + offset, second);
    return internal::hashBytes(bytes, sizeof(bytes));
  }

  template <internal::Byte T> SC_FORCE_INLINE constexpr h64 hash(const T *data, std::integral auto length) noexcept {
    return internal::hashBytes(data, static_cast<size_t>(length));
  }

  template <SizedBuffer T> SC_FORCE_INLINE constexpr h64 hash(const T &buffer) noexcept {
    static_assert(sizeof(*buffer.data()) == 1, "hash() of a buffer needs elements of one byte; pass a "
                                               "pointer and the length in bytes for anything else");
    return internal::hashBytes(buffer.data(), buffer.size());
  }

  // A string literal or a C string. Without this overload they would be taken
  // for neither a number nor a buffer.
  SC_FORCE_INLINE constexpr h64 hash(const char *text) noexcept { return hash(std::string_view{text}); }

  // Like hash(), but for a strongly typed hash value:
  //   enum class ItemId : sc::h64 {};
  //   ItemId id = sc::hash<ItemId>(name);
  template <typename T, typename... Args>
    requires std::is_enum_v<T> && std::same_as<h64, std::underlying_type_t<T>>
  SC_FORCE_INLINE constexpr T hash(Args &&...args) noexcept {
    return static_cast<T>(hash(std::forward<Args>(args)...));
  }

  // The hash of the text with the letters A to Z turned to lowercase.
  SC_FORCE_INLINE constexpr h64 hash_lowercase(std::string_view str) {
    if consteval {
      // A plain array instead of std::string: the checked iterators of some
      // standard libraries use up the compiler's evaluation budget quickly.
      const size_t length = str.size();
      char *const lowered = new char[length + 1];
      for (size_t i = 0; i < length; ++i) {
        const char c = str[i];
        lowered[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
      }
      const h64 result = rapid::constExpr::rapidhash(lowered, length);
      delete[] lowered;
      return result;
    } else {
      return ::rapidhash_lowercase(str.data(), str.size());
    }
  }

  // Transparent hasher for maps whose keys are hashes already, or strings that
  // are looked up by std::string_view.
  struct IdentityHash {
    using is_transparent = void;

    using is_avalanching = void;

    [[nodiscard]] size_t operator()(uint32_t value) const noexcept { return value; }
    [[nodiscard]] size_t operator()(uint64_t v) const noexcept { return v; }
    [[nodiscard]] size_t operator()(std::string_view v) const noexcept { return hash(v); }
    [[nodiscard]] size_t operator()(const std::string &v) const noexcept { return hash(v); }
  };

} // namespace sc

// User-defined literals have to live in the global or an inline namespace to
// be found without a using directive.
inline namespace literals {
  constexpr sc::h64 operator""_h(const char *s, size_t len) { return sc::hash(s, len); }
} // namespace literals
