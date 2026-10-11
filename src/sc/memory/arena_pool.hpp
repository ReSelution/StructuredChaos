//
// Created by oleub on 05.03.26.
//

#pragma once
#include <memory>
#include <mutex>
#include <vector>

#include "sc/memory/monotonic_resource.hpp"
#include "sc/threading/spin_lock.hpp"

namespace sc {

  class ArenaPool {
    static constexpr size_t DEFAULT_SIZE = size_t{16} * 1024;
    static constexpr size_t MAX_LOADED_ARENAS = 1024;

  public:
    static void init(size_t poolSize, size_t arenaSize = DEFAULT_SIZE);

    static std::unique_ptr<MonotonicResource> acquire();

    static void release(std::unique_ptr<MonotonicResource> &resource);
    static size_t getBufferSize();

  private:
    static inline std::vector<std::unique_ptr<MonotonicResource>> m_storage;
    static inline SpinLock m_lock;
    static inline size_t m_defaultSize = DEFAULT_SIZE;
    static inline std::mutex m_initLock;
  };
} // namespace sc
