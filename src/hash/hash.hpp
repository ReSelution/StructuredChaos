//
// Created by oleub on 11.04.26.
//

#pragma once

#include <cstdint>
#include "constexpr-xxh3.h"
#include "xxh3.h"

#if defined(_MSC_VER) // MSVC
#define FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__) // GCC / Clang
#define FORCE_INLINE inline __attribute__((always_inline))
#else
#define FORCE_INLINE inline
#endif

namespace internal {
    consteval uint64_t compute_xxh3(const char *s, size_t len) {
        return constexpr_xxh3::XXH3_64bits_const(s, len);
    }
}

inline namespace literals {
    consteval uint64_t operator ""_h(const char *s, size_t len) {
        return internal::compute_xxh3(s, len);
    }
}


namespace SC {
     FORCE_INLINE uint64_t xxhash(std::string_view str){
         return XXH3_64bits(str.data(), str.size());
     }

    FORCE_INLINE uint64_t xxhash_lowercase(const std::string_view str) {

        // Kleiner Stack-Buffer für Performance
        constexpr size_t MAX_STR_LEN = 512;
        char buffer[MAX_STR_LEN];
        const size_t len = std::min<size_t>(str.length(), MAX_STR_LEN);

        for (size_t i = 0; i < len; ++i) {
            const auto c = static_cast<uint8_t>(str[i]);
            buffer[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c);
        }

        return XXH3_64bits(buffer, len);
    }


    struct IdentityHash {
        using is_transparent = void;

        [[nodiscard]] size_t operator()(uint32_t value) const noexcept {
            return value;
        }

        size_t operator()(uint64_t v) const {
            return v;
        }
    };

    FORCE_INLINE uint64_t hash(std::string_view value) {
        return xxhash(value);
    }

    template<typename T>
    FORCE_INLINE constexpr T hash(std::string_view value) {
        static_assert(std::is_enum_v<T>, "T must be an enum");
        static_assert(std::is_same_v<uint64_t, std::underlying_type_t<T>>, "Enum must underlie uint64_t");

        if consteval {
            return static_cast<T>(internal::compute_xxh3(value.data(), value.size()));
        }
        return static_cast<T>(xxhash(value));

    }


} // SC
