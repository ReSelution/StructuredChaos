#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "sc/config/config.h"

#include "magpie_translator.hpp"

namespace sc {
  // A text by its key, translated whenever it is looked at.
  class MagpieString {
  public:
    MagpieString() = default;

    MagpieString(std::string_view ns, std::string_view key) : m_key(ns, key) {}

    MagpieString(uint64_t ns, uint64_t key) : m_key(ns, key) {}

    explicit MagpieString(MagpieKey key) : m_key(key) {}

    consteval MagpieString(const char *str, size_t len) {
      std::string_view sv{str, len};
#ifdef DUMP_MAGPIE
      m_keyString = sv;
#endif

      auto sep = sv.find(':');
      // The same keys as the constructors that take the texts at run time.
      if (sep == std::string_view::npos) {
        m_key = MagpieKey{sv};
      } else {
        m_key = MagpieKey{sv.substr(0, sep), sv.substr(sep + 1)};
      }
    }

    // The text under its own hash, added to the table as its own source.
    static MagpieString fromStr(std::string_view str) {
      const MagpieKey key{str};
      Magpie::get().insert(key, str, "", str);
      return MagpieString(key);
    }

    // The text of ns:key, which shows src for as long as there is no
    // translation. Adds src to the table.
    static MagpieString withSource(std::string_view ns, std::string_view key, std::string_view src) {
      MagpieString text(ns, key);
      text.m_srcKey = MagpieKey{src};
      Magpie::get().insert(text.m_srcKey, src, "", src);
      return text;
    }

    // The translation, or the source text as long as there is none. Each call
    // looks the text up again, so keep the result instead of calling it twice.
    [[nodiscard]] std::string_view view() const {
      const Magpie &magpie = Magpie::get();
      if (auto text = magpie.find(m_key)) [[likely]] {
        return *text;
      }
      if (m_srcKey != MagpieKey{}) {
        return magpie.translate(m_srcKey);
      }
      return Magpie::MISSING_STRING;
    }

    operator std::string_view() const { return view(); }

    [[nodiscard]] constexpr MagpieKey key() const { return m_key; }
    // Each of these looks the text up on its own. If a translation is merged
    // in between, they may describe two different texts; take view() once
    // where both are needed.
    [[nodiscard]] const char *data() const { return view().data(); }
    [[nodiscard]] size_t size() const { return view().size(); }

  private:
#ifdef DUMP_MAGPIE
    // Only for the debugger. Tied to dump_magpie rather than NDEBUG, which may
    // differ between translation units; this config is the same everywhere.
    std::string_view m_keyString;
#endif
    MagpieKey m_key;
    MagpieKey m_srcKey;
  };

  inline namespace literals {
    consteval MagpieString operator""_t(const char *str, size_t len) { return {str, len}; }
  } // namespace literals
} // namespace sc
