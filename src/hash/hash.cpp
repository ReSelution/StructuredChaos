//
// Created by oleub on 11.04.26.
//

#include <string_view>
#include "hash.hpp"
#include "city.h"
#include <hwy/highway.h>
#include "xxh3.h"

namespace SC::hash {
    uint64_t xxhash_lowercase(const std::string_view str) {
        namespace hn = hwy::HWY_NAMESPACE;

        // Kleiner Stack-Buffer für Performance
        constexpr size_t MAX_STR_LEN = 512;
        char buffer[MAX_STR_LEN];
        const size_t len = std::min<size_t>(str.length(), MAX_STR_LEN);

        for (size_t i = 0; i < len; ++i) {
            const uint8_t c = static_cast<uint8_t>(str[i]);
            buffer[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c);
        }

        return XXH3_64bits(buffer, len);
    }

    uint64_t xxhash(const std::string_view str) {
        return XXH3_64bits(str.data(), str.size());
    }



} // SC