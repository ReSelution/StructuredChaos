//
// Created by oleub on 05.03.26.
//

#include "chaos_bump_arena.hpp"
#include "simdutf.h"

namespace SC {
  ChaosBumpArena::ChaosBumpArena(const size_t size) : m_capacity(size),
                                            m_backingBuffer(
                                              std::make_unique_for_overwrite<uint8_t[]>(m_capacity)),
                                            m_pool(m_backingBuffer.get(), m_capacity) {
  }


  std::string_view ChaosBumpArena::utf16ToUtf8(const std::span<const char16_t> input) {
    if (input.empty())[[unlikely]] return "";

    size_t requiredSize = simdutf::utf8_length_from_utf16(input.data(), input.size());
    char* target = static_cast<char*>(allocate(requiredSize + 1, alignof(char)));
    auto size = simdutf::convert_utf16_to_utf8(input.data(), input.size(), target);
    assert(size == requiredSize);
    target[requiredSize] = '\0';
    return {target, requiredSize};
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
