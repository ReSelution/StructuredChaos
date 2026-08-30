

module;

#include <algorithm>
#include <cstddef>
#include <string_view>
export module sc.util:fixed_string;

namespace sc {
  export template<size_t N>
  struct FixedString {
    char buf[N]{};
    constexpr FixedString(const char *str) { std::copy_n(str, N, buf); }
    constexpr operator std::string_view() const { return {buf, N - 1}; }
    constexpr std::string_view text() const { return std::string_view(buf, N - 1); }
  };
  template<size_t N>
  FixedString(const char (&)[N]) -> FixedString<N>;

} // namespace sc
