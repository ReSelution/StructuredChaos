module;

#include <cstddef>
#include <simdutf.h>

export module SC.Util:utf;

namespace SC::utf8 {

export [[nodiscard]] inline size_t length(const char16_t *in,
                                          size_t len) noexcept {
  return simdutf::utf8_length_from_utf16(in, len);
}

export inline size_t convert(const char16_t *in, size_t len,
                             char *out) noexcept {
  return simdutf::convert_utf16_to_utf8(in, len, out);
}

export [[nodiscard]] inline bool valid(const char16_t *in,
                                       size_t len) noexcept {
  return simdutf::validate_utf16(in, len);
}

} // namespace SC::utf8
