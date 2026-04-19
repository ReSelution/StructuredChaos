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

    MagpieString(std::string_view ns, std::string_view key) : m_key(ns, key) {
    }

    MagpieString(uint64_t ns, uint64_t key) : m_key(ns, key) {
    }

    consteval MagpieString(const char *str, size_t len) {
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
  };
} // SC

consteval SC::MagpieString operator ""_t(const char *str, size_t len) {
  return {str, len};
}
