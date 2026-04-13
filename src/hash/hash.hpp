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

    template<typename ... Args>
    FORCE_INLINE constexpr h64 hash(Args &&...args) {
        if constexpr (sizeof...(Args) == 2) {
            if consteval {
                auto extract = [](auto &&first, auto &&second) constexpr {
                    return rapid::constExpr::rapidhash(first, static_cast<size_t>(second));
                };
                return extract(std::forward<Args>(args)...);
            }
            else {
                auto extract = [](auto &&first, auto &&second) {
                    return rapidhash(reinterpret_cast<const uint8_t *>(first), static_cast<size_t>(second));
                };
                return extract(std::forward<Args>(args)...);
            }
        } else if constexpr (sizeof...(Args) == 1) {
            auto &&arg = std::get<0>(std::forward_as_tuple(std::forward<Args>(args)...));

            using ArgType = std::decay_t<decltype(arg)>;

            if constexpr (std::is_integral_v<ArgType>) {
                auto val = static_cast<uint64_t>(arg);
                if consteval {
                    return rapid::constExpr::rapidhash(reinterpret_cast<const uint8_t *>(&val), sizeof(val));
                }
                else {
                    return rapidhash(reinterpret_cast<const uint8_t *>(&val), sizeof(val));
                }
            } else {
                auto dispatch = [](auto &&a) constexpr {
                    if constexpr (std::is_convertible_v<decltype(a), std::string_view>) {
                        std::string_view sv = a;
                        if consteval {
                            return rapid::constExpr::rapidhash(sv.data(), sv.size());
                        }
                        else {
                            return rapidhash(reinterpret_cast<const uint8_t *>(sv.data()), sv.size());
                        }
                    } else {
                        if consteval {
                            return rapid::constExpr::rapidhash(a.data(), a.size());
                        }
                        else {
                            return rapidhash(reinterpret_cast<const uint8_t *>(a.data()), a.size());
                        }
                    }
                };

                return dispatch(arg);
            }
        }
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
        }
        else {
            return rapidhash_lowercase(str.data(), str.size());
        }
    }


    template<typename T, typename ... Args>
    FORCE_INLINE constexpr T hash(Args &&...args) {
        static_assert(std::is_enum_v<T>, "T must be an enum");
        static_assert(std::is_same_v<h64, std::underlying_type_t<T>>, "Enum must underlie uint64_t");
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
            return hash(v);
        }

        [[nodiscard]] size_t operator()(std::string v) const noexcept {
            return hash(v);
        }
    };

} // SC
inline namespace literals {
    constexpr SC::h64 operator ""_h(const char *s, size_t len) {
        return SC::hash(s, len);
    }
}