//
// Created by oleub on 05.03.26.
//

#include "chaos_bump_arena.hpp"

namespace SC {
  ChaosBumpArena::ChaosBumpArena(const size_t size) : m_capacity(size),
                                            m_backingBuffer(
                                              std::make_unique_for_overwrite<uint8_t[]>(m_capacity)),
                                            m_pool(m_backingBuffer.get(), m_capacity) {
  }


  std::string_view ChaosBumpArena::utf16ToUtf8(const std::span<const char16_t> input) {
    if (input.empty()) return "";

    size_t maxLen = input.size() * 3;
    char *target = static_cast<char *>(allocate(maxLen + 1, alignof(char)));
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

  void ChaosBumpArena::reset() {
    CleanupNode* head = m_cleanupHead.exchange(nullptr, std::memory_order_acq_rel);
    while (head) {
      head->destroyer(head->object);
      head = head->next;
    }
    m_pool.release();
  }

  void *ChaosBumpArena::allocate(const size_t size, const size_t align) {
    return  m_pool.allocate(size, align);
  }
} // SC
