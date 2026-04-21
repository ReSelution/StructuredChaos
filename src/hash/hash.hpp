//
// Created by oleub on 11.04.26.
//

#pragma once

#include <utility>
#include <tuple>
#include <type_traits>
#include <string_view>
#include "rapidhash.h"
#include "rapidhash-constexpr.h"


#if defined(_MSC_VER) // MSVC
#define FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__) // GCC / Clang
#define FORCE_INLINE inline __attribute__((always_inline))
#else
#define FORCE_INLINE inline
#endif


namespace SC {
  using h64 = uint64_t;

  template<typename T>
 concept SizedBuffer = requires(T a) {
    // 1. Prüfe ob die Methoden überhaupt existieren
    a.data();
    a.size();

    { a.data() } -> std::same_as<decltype(a.data())>; // Validiert, dass es aufrufbar ist
    requires std::is_pointer_v<decltype(a.data())>;  // Muss ein Pointer sein
    { a.size() } -> std::convertible_to<std::size_t>; // Muss Größe liefern
 };

  template<typename... Args>
  FORCE_INLINE constexpr h64 hash(Args &&... args) {
    // Case 1 2x Integers or ptr + lenght
    if constexpr (sizeof...(Args) == 2) {
      auto [a1, a2] = std::forward_as_tuple(std::forward<Args>(args)...);
      using T1 = std::decay_t<decltype(a1)>;
      using T2 = std::decay_t<decltype(a2)>;

      if constexpr (std::is_integral_v<T1> && std::is_integral_v<T2>) {
        uint8_t buffer[16]{};
        size_t len = 0;

        if consteval {
          auto copy = [&](auto val) {
            for (size_t i = 0; i < sizeof(val); ++i)
              buffer[len++] = static_cast<uint8_t>((val >> (i * 8)) & 0xFF);
          };
          copy(a1);
          copy(a2);
          return rapid::constExpr::rapidhash(buffer, len);
        } else {
          std::memcpy(buffer, &a1, sizeof(a1));
          std::memcpy(buffer + sizeof(a1), &a2, sizeof(a2));
          return rapidhash(buffer, sizeof(a1) + sizeof(a2));
        }
      } else if constexpr (std::is_pointer_v<T1> && std::is_integral_v<T2>) {
        if consteval {
          return rapid::constExpr::rapidhash(a1, static_cast<size_t>(a2));
        } else {
          return rapidhash(reinterpret_cast<const uint8_t *>(a1), static_cast<size_t>(a2));
        }
      }
      static_assert(
        std::is_pointer_v<T1> && std::is_integral_v<T2> || (std::is_integral_v<T1> && std::is_integral_v<T2>));
    }
    // 2. Fall: Ein Argument (String-like oder einzelner Integer)
    // Case 2. str
    else if constexpr (sizeof...(Args) == 1) {
      auto &&arg = std::get<0>(std::forward_as_tuple(std::forward<Args>(args)...));
      using T = std::decay_t<decltype(arg)>;

      if constexpr (std::is_integral_v<T>) {
        static_assert(sizeof(T) <= sizeof(uint64_t), "T must be uint64_t or Lower");
        if consteval {
          uint8_t b[sizeof(T)];
          for (size_t i = 0; i < sizeof(T); ++i) {
            b[i] = static_cast<uint8_t>((arg >> (i * 8)) & 0xFF);
          }
          return rapid::constExpr::rapidhash(b, sizeof(T));
        } else {
          return rapidhash(reinterpret_cast<const uint8_t *>(&arg), sizeof(T));
        }
      } else if constexpr (SizedBuffer<T>) {
        if consteval {
          return rapid::constExpr::rapidhash(arg.data(), arg.size());
        } else {
          return rapidhash(reinterpret_cast<const uint8_t*>(arg.data()), arg.size());
        }
      }
    }
    return 0;
  }

FORCE_INLINE constexpr h64 hash_lowercase(std::string_view str) {
  if consteval {
    constexpr size_t MAX_STR_LEN = 512;
    uint8_t buffer[MAX_STR_LEN];
    const size_t len = std::min<size_t>(str.length(), MAX_STR_LEN);

    for (size_t i = 0; i < len; ++i) {
      const auto c = static_cast<uint8_t>(str[i]);
      buffer[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c);
    }
    return rapid::constExpr::rapidhash(buffer, len);
  } else {
    return rapidhash_lowercase(str.data(), str.size());
  }
}


template<typename T, typename... Args>
FORCE_INLINE constexpr T hash(Args &&... args) {
  static_assert(std::is_enum_v<T>, "T must be an enum");
  static_assert(std::is_same_v<h64, std::underlying_type_t<T> >, "Enum must underlie uint64_t");
  return static_cast<T>(hash(std::forward<Args>(args)...));
}

struct IdentityHash {
  using is_transparent = void;
  using is_avalanching = void;

  [[nodiscard]] size_t operator()(uint32_t value) const noexcept {
    return value;
  }

  size_t operator()(uint64_t v) const {
    return v;
  }

  [[nodiscard]] size_t operator()(std::string_view v) const noexcept {
    return SC::hash(v);
  }

  [[nodiscard]] size_t operator()(std::string v) const noexcept {
    return SC::hash(v);
  }
};

} // SC
inline namespace literals {
  constexpr SC::h64 operator ""_h(const char *s, size_t len) {
    return SC::hash(s, len);
  }
}
