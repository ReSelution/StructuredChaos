//
// Created by oleub on 05.03.26.
//

#pragma once
#include <vector>
#include <atomic>
#include <memory>

#include "chaos_slab.hpp"

namespace SC {
  class ChaosSlabPool {
    static constexpr size_t DEFAULT_SIZE = 16 * 1024;

  public:
    static void init(size_t poolSize, size_t arenaSize = DEFAULT_SIZE);

    static std::unique_ptr<ChaosSlab> acquire();

    static void release(std::unique_ptr<ChaosSlab> &arena);

  private:
    static inline std::vector<std::unique_ptr<ChaosSlab> > m_storage;
    static inline std::atomic_flag m_lock = ATOMIC_FLAG_INIT;
    static inline size_t m_defaultSize = DEFAULT_SIZE;
  };
}
