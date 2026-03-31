//
// Created by oleub on 31.03.26.
//

#pragma once
#include <bit>
#include <cstdint>
#include <array>
#include <atomic>


namespace SC {
  template<size_t GrainSize>
  requires (std::has_single_bit<size_t>(GrainSize))
  class ChaosMemoryCell {
    static constexpr size_t TOTAL_SIZE = 32768;

    static constexpr size_t GRAIN_COUNT = TOTAL_SIZE / GrainSize;
    static constexpr size_t RequiredMasks = (GRAIN_COUNT + 63) / 64;


    struct Header {
      uint32_t magic = 0x000C4A05;
      uint32_t flags = 0;
      size_t freeSpace = 0;
      std::array<std::atomic<uint64_t>, RequiredMasks> metadata;
    };

    union {
      Header header;
      uint8_t raw[TOTAL_SIZE];
    } m_storage;

    static constexpr size_t dataOffset() {
      return (sizeof(Header) + GrainSize - 1) & ~(GrainSize - 1);
    }

    void * allocate(size_t size, size_t align);
  };

  template<size_t GrainSize> requires (std::has_single_bit<unsigned long>(GrainSize))
  void * ChaosMemoryCell<GrainSize>::allocate(size_t size, size_t align) {
    const size_t neededGrains = (size + GrainSize - 1) / GrainSize;
    if (neededGrains > 64) [[unlikely]] return nullptr;

    for (size_t i = 0; i < RequiredMasks; ++i) {
        uint64_t currentMask = metadata[i].load(std::memory_order_relaxed);

        if (currentMask == ~0ULL) continue;

        uint64_t combined = currentMask;
        for (size_t s = 1; s < neededGrains; ++s) {
            combined |= (currentMask << s);

        }

        uint64_t freeStarts = ~combined;

        while (freeStarts != 0) {
            int bitIdx = std::countr_zero(freeStarts);

            uint64_t blockMask = ((1ULL << neededGrains) - 1) << bitIdx;


            uint64_t expected = currentMask;
            if (m_storage.header.metadata[i].compare_exchange_weak(expected, currentMask | blockMask,
                                                 std::memory_order_acquire,
                                                 std::memory_order_relaxed)) {

                size_t globalGrainIdx = (i * 64) + bitIdx;

                constexpr size_t dataOffset = (sizeof(Header) + GrainSize - 1) & ~(GrainSize - 1);
                uint8_t* basePtr = reinterpret_cast<uint8_t*>(this) + dataOffset;

                return basePtr + (globalGrainIdx * GrainSize);
            }

            currentMask = m_storage.header.metadata[i].load(std::memory_order_relaxed);
            freeStarts &= ~(1ULL << bitIdx);
        }
    }

    return nullptr; // Kein Platz gefunden
  }
} // SC