#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <string_view>

namespace sc {
  // A string literal as a template argument, e.g. Logger<"Net">.
  template<size_t N>
  struct FixedString {
    char buf[N]{};

    constexpr FixedString(const char (&str)[N]) {
      std::copy_n(str, N, buf);
      assert(buf[N - 1] == '\0' && "sc::FixedString: the text has to end with its terminator");
    }

    [[nodiscard]] constexpr std::string_view text() const { return {buf, N - 1}; }
    constexpr operator std::string_view() const { return text(); }
  };
  template<size_t N>
  FixedString(const char (&)[N]) -> FixedString<N>;

} // namespace sc
