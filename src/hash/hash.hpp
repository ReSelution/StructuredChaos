//
// Created by oleub on 11.04.26.
//

#pragma once

#include <utility>
#include <tuple>
#include <type_traits>
#include <string_view>
#include "rapidhash/rapidhash.h"


#if defined(_MSC_VER) // MSVC
#define FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__) // GCC / Clang
#define FORCE_INLINE inline __attribute__((always_inline))
#else
#define FORCE_INLINE inline
#endif


inline namespace literals {
    consteval uint64_t operator ""_h(const char *s, size_t len) {
        return rapidhash(s, len);
    }
}


namespace SC {
    using h64 = uint64_t;

    struct IdentityHash {
        using is_transparent = void;

        [[nodiscard]] size_t operator()(uint32_t value) const noexcept {
            return value;
        }

        size_t operator()(uint64_t v) const {
            return v;
        }
    };

    template<typename ... Args>
    FORCE_INLINE constexpr h64 hash(Args &&...args) {
        if constexpr (sizeof ... (Args) == 2) {
            auto extract = [](auto &&first, auto &&second) {
                return rapidhash(static_cast<const void *>(first), static_cast<size_t>(second));
            };
            return extract(std::forward<Args>(args)...);
        } else if constexpr (sizeof ...(Args) == 1) {
            auto &&arg = std::get<0>(std::forward_as_tuple(std::forward<Args>(args)...));

            return rapidhash(arg.data(), arg.size());
        }
    }

    FORCE_INLINE constexpr h64 hash_lowercase(std::string_view str) {
        constexpr size_t MAX_STR_LEN = 512;
        char buffer[MAX_STR_LEN];
        const size_t len = std::min<size_t>(str.length(), MAX_STR_LEN);

        for (size_t i = 0; i < len; ++i) {
            const auto c = static_cast<uint8_t>(str[i]);
            buffer[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c);
        }
        return rapidhash(buffer, len);
    }
    

    template<typename T, typename ... Args>
    FORCE_INLINE constexpr T hash(Args &&...args) {
        static_assert(std::is_enum_v<T>, "T must be an enum");
        static_assert(std::is_same_v<h64, std::underlying_type_t<T>>, "Enum must underlie uint64_t");
        return static_cast<T>(hash(std::forward<Args>(args)...));
    }


} // SC
