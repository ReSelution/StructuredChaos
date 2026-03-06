//
// Created by oleub on 05.03.26.
//

#include "chaos_slab.hpp"

namespace SC {
  ChaosSlab::ChaosSlab(const size_t size) : m_capacity(size),
                                            m_backingBuffer(
                                              std::make_unique_for_overwrite<uint8_t[]>(m_capacity)),
                                            m_pool(m_backingBuffer.get(), size) {
  }


  std::string_view ChaosSlab::utf16ToUtf8(const std::span<const char16_t> input) {
    if (input.empty()) return "";

    size_t maxLen = input.size() * 3;
    char *target = static_cast<char *>(internal_allocate(maxLen + 1, alignof(char)));
    char *curr = target;

    for (uint16_t cp: input) {
      if (cp < 0x80) {
        *curr++ = static_cast<char>(cp);
      } else if (cp < 0x800) {
        *curr++ = static_cast<char>(0xC0 | (cp >> 6));
        *curr++ = static_cast<char>(0x80 | (cp & 0x3F));
      } else {
        *curr++ = static_cast<char>(0xE0 | (cp >> 12));
        *curr++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *curr++ = static_cast<char>(0x80 | (cp & 0x3F));
      }
    }
    *curr = '\0';
    return {target, static_cast<size_t>(curr - target)};
  }

  void ChaosSlab::reset() {
    while (m_cleanupHead) {
      m_cleanupHead->destroyer(m_cleanupHead->object);
      m_cleanupHead = m_cleanupHead->next;
    }
    m_pool.release();
  }

  void *ChaosSlab::internal_allocate(const size_t size, const size_t align) {
    return m_pool.allocate(size, align);
  }
} // SC
