//
// Created by oleub on 18.04.26.
//

#pragma once
#include <cstdint>
#include <string_view>

#include "magpie_translator.hpp"

namespace SC {
  class MagpieString {
  public:
    MagpieString() = default;

    MagpieString(std::string_view ns, std::string_view key) : m_key(ns, key), m_srcKey() {
    }

    MagpieString(std::string_view ns, std::string_view key, std::string_view src) : m_key(ns, key), m_srcKey(src) {
      Magpie::get()->insert(m_srcKey, src, "", src);
    }

    MagpieString(uint64_t ns, uint64_t key) : m_key(ns, key), m_srcKey() {
    }

    explicit MagpieString(MagpieKey key) : m_key(key),m_srcKey() {
    }

    consteval MagpieString(const char *str, size_t len): m_srcKey() {
      std::string_view sv{str, len};
#ifndef NDEBUG
      m_keyString = sv;
#endif

      auto sep = sv.find(':');
      if (sep == std::string_view::npos) {
        m_key = MagpieKey{hash(sv.data(), sv.size())};
      } else {
        m_key = MagpieKey{hash(sv.data(), sep), hash(sv.data() + sep + 1, len - sep - 1)};
      }
    }

    static MagpieString fromStr(std::string_view str) {
      MagpieKey key{str};
      Magpie::get()->insert(key, str, "", str);
      return MagpieString(key);
    };

    [[nodiscard]] std::string_view view() const noexcept {
      return Magpie::get()->translate(m_key);
    }

    operator std::string_view() const { return view(); }

    [[nodiscard]] MagpieKey key() const { return m_key; }
    [[nodiscard]] const char *data() const noexcept { return view().data(); }
    [[nodiscard]] size_t size() const noexcept { return view().size(); }

  private:
#ifndef NDEBUG
    std::string_view m_keyString;
#endif
    MagpieKey m_key;
    MagpieKey m_srcKey;
  };
} // SC

consteval SC::MagpieString operator ""_t(const char *str, size_t len) {
  return {str, len};
}
