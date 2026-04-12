//
// Created by oleub on 11.04.26.
//

#pragma once
#include <cstdint>
#include "constexpr-xxh3.h"

namespace internal {
    consteval uint64_t compute_xxh3(const char* s, size_t len) {
        return constexpr_xxh3::XXH3_64bits_const(s, len);
    }
}

inline namespace literals {
    consteval uint64_t operator ""_h(const char* s, size_t len) {
        return internal::compute_xxh3(s, len);
    }
}


namespace SC::hash {
    uint64_t xxhash(const std::string_view str);
    uint64_t xxhash_lowercase(const std::string_view str);

    uint64_t cityHash64_ue(const std::string_view str);

    struct IdentityHash {
        using is_transparent = void;

        [[nodiscard]] size_t operator()(uint32_t value) const noexcept {
            return value;
        }

        size_t operator()(uint64_t v) const {
            return v;
        }
    };

    template<typename T>
    constexpr T hash(std::string_view value) {
        static_assert(std::is_enum<T>::value, "T must be an enum");
        static_assert(std::same_as<uint64_t,std::underlying_type_t<T>>);
        if  (std::is_constant_evaluated()) {
            return static_cast<T>( internal::compute_xxh3(value.data(),value.size()));
        } else{
            return static_cast<T>(xxhash(value));
        }
    }


} // SC
