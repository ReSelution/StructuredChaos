//
// Created by oleub on 05.03.26.
//

#pragma once
#include <memory>
#include <vector>

#include "arena.hpp"
#include "threading/spin_lock.hpp"

namespace sc {

class ArenaPool {
  static constexpr size_t DEFAULT_SIZE = 16 * 1024;
  static constexpr size_t MAX_LOADED_ARENAS = 1024;

public:
  static void init(size_t poolSize, size_t arenaSize = DEFAULT_SIZE);

  static std::unique_ptr<Arena> acquire();

  static void release(std::unique_ptr<Arena> &arena);
  static size_t getBufferSize();

private:
  static inline std::vector<std::unique_ptr<Arena>> m_storage;
  static inline SpinLock m_lock;
  static inline size_t m_defaultSize = DEFAULT_SIZE;
};
} // namespace sc
