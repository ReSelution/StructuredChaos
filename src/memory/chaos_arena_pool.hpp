//
// Created by oleub on 05.03.26.
//

#pragma once
#include <vector>
#include <memory>

#include "chaos_bump_arena.hpp"
#include "threading/chaos_spin_lock.hpp"



namespace SC {

  class ChaosArenaPool {
    static constexpr size_t DEFAULT_SIZE = 16 * 1024;
    static constexpr size_t MAX_LOADED_ARENAS = 256;

  public:
    static void init(size_t poolSize, size_t arenaSize = DEFAULT_SIZE);

    static std::unique_ptr<ChaosBumpArena> acquire();

    static void release(std::unique_ptr<ChaosBumpArena> &arena);
    static size_t getBufferSize();

  private:
    static inline std::vector<std::unique_ptr<ChaosBumpArena> > m_storage;
    static inline ChaosSpinLock m_lock;
    static inline size_t m_defaultSize = DEFAULT_SIZE;
  };
}
