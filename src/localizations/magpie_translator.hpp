//
// Created by oleub on 18.04.26.
//

#pragma once

#include <string_view>

#include "ankerl/unordered_dense.h"
#include "hash/hash.hpp"
#include "memory/chaos_bump_arena.hpp"
#include "threading/chaos_spin_lock.hpp"

namespace SC {
  DEFINE_CHAOS_STAT(MagpieInsert, "Magpie Throughput", SC::ChaosThroughput<SC::MetricUnits>);
  DEFINE_CHAOS_STAT(MagpieMEM, "Magpie Memory", SC::ChaosThroughput<>);

  struct MagpieKey {
    uint64_t key = 0;
#ifdef DUMP_MAGPIE
    std::string_view ns_str{};
    std::string_view key_str{};
#endif

    MagpieKey() = default;

    constexpr  MagpieKey(uint64_t key) : key(key) {
    }

    constexpr MagpieKey(std::string_view ns, std::string_view key) : MagpieKey(SC::hash(ns), SC::hash(key)) {
    }

    constexpr  MagpieKey(std::string_view key) : MagpieKey(0, SC::hash(key)) {
    }


    constexpr  MagpieKey(const uint64_t ns, uint64_t k): key(SC::hash(ns, k)) {
    }

    bool operator==(const MagpieKey &other) const = default;
  };

  struct MagpieKeyHasher {
    using is_avalanching = void;

    size_t operator()(const MagpieKey &k) const noexcept {
      return k.key;
    }
  };

  using magpieMAP = ankerl::unordered_dense::map<MagpieKey, std::string_view, MagpieKeyHasher>;

  class Magpie {
  public:
    static Magpie *get() {
      static Magpie magpie{};
      return &magpie;
    }

    std::string_view translate(const MagpieKey key) noexcept {
      std::shared_lock lock(sh_mtx);
      if (auto it = entries.find(key); it != entries.end())[[likely]] {
        return it->second;
      }
      return "<Magpie missing String>";
    }

    static void mt_reserve(size_t size) {
      tl_map.reserve(size);
    }

    void insert(MagpieKey &key, std::string_view valueStr, std::string_view ns, std::string_view keyStr);


    void mt_Insert(MagpieKey key, std::string_view valueStr, std::string_view ns, std::string_view keyStr );


    void mt_Merge(bool override);

    [[nodiscard]] size_t size() const noexcept {
      return entries.size();
    }

    void clear() noexcept;
    void dump() noexcept;
    void dumpToFile(std::string_view file) noexcept;

  private:

    std::string_view storeStr(std::string_view str);
    static thread_local magpieMAP tl_map; // Because MINGW
    // MINGWs problem: static inline thread_local magpieMAP tl_map{};
    alignas(64) std::shared_mutex sh_mtx;
    magpieMAP entries{};
    ChaosBumpArena m_storage{1024 * 1024};
  };
}
