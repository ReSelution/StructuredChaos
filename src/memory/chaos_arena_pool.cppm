module;

#include <cstddef>
#include <memory>
#include <vector>

export module SC.Memory:ArenaPool;

import :Arena;
import SC.Threading;

// Falls das Log-Makro oder dein Spinlock noch Header sind:

namespace SC {

export class ChaosArenaPool {
  static constexpr size_t DEFAULT_SIZE = 16 * 1024;
  static constexpr size_t MAX_LOADED_ARENAS = 1024;

public:
  static void init(size_t poolSize, size_t arenaSize = DEFAULT_SIZE) {
    m_defaultSize = arenaSize;
    m_storage.reserve(MAX_LOADED_ARENAS);

    for (size_t i = 0; i < poolSize; ++i) {
      m_storage.push_back(new ChaosArena(m_defaultSize));
    }
  }

  [[nodiscard]] static ChaosArena *acquire() noexcept {
    m_lock.lock();
    if (m_storage.empty()) [[unlikely]] {
      m_lock.unlock();
      return new ChaosArena(m_defaultSize);
    }

    auto *arena = m_storage.back();
    m_storage.pop_back();
    m_lock.unlock();
    return arena;
  }

  static void release(ChaosArena *arena) noexcept {
    if (!arena) [[unlikely]]
      return;

    arena->reset();

    m_lock.lock();
    if (m_storage.size() >= MAX_LOADED_ARENAS) {
      m_lock.unlock();
      delete arena;
      return;
    }

    m_storage.push_back(arena);
    m_lock.unlock();
  }

  [[nodiscard]] static size_t getBufferSize() noexcept { return m_defaultSize; }

  static void shutdown() noexcept {
    m_lock.lock();
    for (auto *arena : m_storage) {
      delete arena;
    }
    m_storage.clear();
    m_lock.unlock();
  }

private:
  static inline std::vector<ChaosArena *> m_storage;
  static inline ChaosSpinLock m_lock;
  static inline size_t m_defaultSize = DEFAULT_SIZE;
};

} // namespace SC
