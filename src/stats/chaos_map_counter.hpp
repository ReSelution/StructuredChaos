#pragma once
#include <atomic>
#include <unordered_map>
#include <string>
#include <format>
#include <algorithm>

namespace SC {

  template<typename KeyType = std::string>
  struct ChaosMapCounter {
    using Key = KeyType;

    struct Storage {
      std::unordered_map<Key, uint64_t> counts;
      std::atomic_flag lock = ATOMIC_FLAG_INIT;
    };

    // RAII-Guard für den Spinlock
    struct SpinLockGuard {
      std::atomic_flag& flag;
      SpinLockGuard(std::atomic_flag& f) : flag(f) {
        while (flag.test_and_set(std::memory_order_acquire)) {
#if defined(__x86_64__) || defined(_M_X64)
          __builtin_ia32_pause();
#endif
        }
      }
      ~SpinLockGuard() { flag.clear(std::memory_order_release); }
    };

    static void record(Storage& s, const Key& key, uint64_t amount = 1) {
      SpinLockGuard guard(s.lock);
      s.counts[key] += amount;
    }

    static void reset(Storage& s) {
      SpinLockGuard guard(s.lock);
      s.counts.clear();
    }

    static std::string format(const Storage& s) {
      // Wir müssen hier lügen, da format const ist, aber der Lock nicht.
      // In diesem Kontext (Reporting) ist ein const_cast vertretbar.
      auto& mutable_s = const_cast<Storage&>(s);
      SpinLockGuard guard(mutable_s.lock);

      uint64_t total = 0;
      for (const auto& [_, count] : s.counts) total += count;

      return std::format("Unique: {} | Total: {}", s.counts.size(), total);
    }
  };

} // namespace SC