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

      auto& mutable_s = const_cast<Storage&>(s);
      SpinLockGuard guard(mutable_s.lock);

      if (s.counts.empty()) return "No data recorded";

      uint64_t total = 0;
      std::vector<std::pair<Key, uint64_t>> sorted;
      sorted.reserve(s.counts.size());

      for (const auto& [key, count] : s.counts) {
        total += count;
        sorted.push_back({key, count});
      }

      std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
          return a.second > b.second;
      });

      std::string details;
      size_t limit = std::min<size_t>(15, sorted.size());
      for (size_t i = 0; i < limit; ++i) {
        details += std::format("    {}.\t-> {} ({}){}", i+1,sorted[i].first, sorted[i].second, (i < limit - 1 ? ",\n" : ""));
      }
      return std::format("Unique: {} | Total: {} | Top: {} ({}) \n{}", s.counts.size(), total, sorted[0].first, sorted[0].second,details);
    }
  };

} // namespace SC