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

        const hn::ScalableTag<uint8_t> d8;
        const auto upper_A = hn::Set(d8, 'A');
        const auto upper_Z = hn::Set(d8, 'Z');
        const auto to_lower_offset = hn::Set(d8, 32);

        size_t i = 0;
        for (; i + hn::Lanes(d8) <= len; i += hn::Lanes(d8)) {
            auto v = hn::LoadU(d8, reinterpret_cast<const uint8_t*>(&str[i]));

            auto is_upper = hn::And(hn::Ge(v, upper_A), hn::Le(v, upper_Z));

            auto offset_vec = hn::IfThenElseZero(is_upper, to_lower_offset);
            auto low_v = hn::Add(v, offset_vec);

            hn::StoreU(low_v, d8, reinterpret_cast<uint8_t*>(&buffer[i]));
        }

        for (; i < len; ++i) {
            const uint8_t c = static_cast<uint8_t>(str[i]);
            buffer[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c);
        }

        return XXH3_64bits(buffer, len);
    }

    uint64_t xxhash(const std::string_view str){
        return XXH3_64bits(str.data(), str.size());
    }

    uint64_t cityHash64_ue(const std::string_view str) {
        namespace hn = hwy::HWY_NAMESPACE;
        constexpr  uint32_t MAX_STR_LEN = 256;
        char16_t buffer[MAX_STR_LEN];
        const size_t len = std::min<size_t>(str.length(), MAX_STR_LEN);

        const hn::ScalableTag<uint8_t> d8;   // Tag für 8-bit chars
        const hn::ScalableTag<uint16_t> d16; // Tag für 16-bit chars (UTF16)

        size_t i = 0;
        for (; i + hn::Lanes(d8) <= len; i += hn::Lanes(d8)) {
            auto v = hn::LoadU(d8, reinterpret_cast<const uint8_t*>(&str[i]));

            auto is_upper = hn::And(hn::Ge(v, hn::Set(d8, 'A')),
                                    hn::Le(v, hn::Set(d8, 'Z')));
            auto low_v = hn::Add(v, hn::IfThenElseZero(is_upper, hn::Set(d8, 32)));

            auto part1 = hn::PromoteLowerTo(d16, low_v);
            auto part2 = hn::PromoteUpperTo(d16, low_v);

            hn::StoreU(part1, d16, reinterpret_cast<uint16_t*>(&buffer[i]));
            hn::StoreU(part2, d16, reinterpret_cast<uint16_t*>(&buffer[i + hn::Lanes(d16)]));
        }

        for (;i < len; ++i) {
            const auto uc = static_cast<char>(str[i]);
            char lc = (uc >= 'A' && uc <= 'Z') ? static_cast<char>(uc + 32) : uc;
            buffer[i] = static_cast<char16_t>(lc);
        }

        return CityHash64(reinterpret_cast<const char *>(buffer), len * sizeof(char16_t));
    }



} // SC