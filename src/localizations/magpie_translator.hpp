//
// Created by oleub on 18.04.26.
//

#pragma once

#include <string_view>

#include "ankerl/unordered_dense.h"
#include "hash/hash.hpp"
#include "memory/chaos_bump_arena.hpp"

namespace SC {

  DEFINE_CHAOS_STAT(MagpieInsert, "Magpie Throughput",  SC::ChaosThroughput<SC::MetricUnits>);
  DEFINE_CHAOS_STAT(MagpieMEM, "Magpie Memory",  SC::ChaosThroughput<>);

  struct MagpieKey {
    uint64_t key;

    MagpieKey() = default;
    MagpieKey(uint64_t key) : key(key) {
    }

    MagpieKey(std::string_view ns, std::string_view key) : MagpieKey(SC::hash(ns), SC::hash(key)) {
    }


    MagpieKey(const uint64_t ns, uint64_t key)  {
      const uint64_t data[2] = {ns, key};
      this->key = SC::hash(reinterpret_cast<const uint8_t *>(data), sizeof(data));
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


    static Magpie* get() {
      static Magpie magpie{};
      return &magpie;
    }

    std::string_view translate(const MagpieKey key) noexcept {
      if (auto it = entries.find(key); it != entries.end()) {
        return it->second;
      }
      return "<Magpie missing String>";
    };
    void mt_reserve(size_t size) {
      tl_map.reserve(size);
    }
    void mt_Insert(MagpieKey key, std::string_view str) {

      auto s = m_storage.allocateSpan<char>(str.size());
      memcpy(s.data(), str.data(), s.size());
      tl_map.emplace(key, s);
      MagpieMEM::record(str.size());
    }

    void mt_Merge(bool override) {
      if (override) {
        std::lock_guard<std::mutex> lock(m_mutex);
        entries.reserve(entries.size() + tl_map.size());
        for (auto [key, value] : tl_map) {
          entries.insert_or_assign(key, value);
        }
      }
      else {
        std::lock_guard<std::mutex> lock(m_mutex);
        entries.reserve(entries.size() + tl_map.size());
        entries.insert(tl_map.begin(), tl_map.end());
      }
      MagpieInsert::record(tl_map.size());
      tl_map.clear();
    }

    size_t size() const noexcept {
      return entries.size();
    }

    void clear() noexcept {
      CHAOS_RESET(MagpieInsert)
      CHAOS_RESET(MagpieMEM)
      entries.clear();
      m_storage.reset();
    }

  private:
    static inline  thread_local magpieMAP tl_map{};
    std::mutex m_mutex;
    magpieMAP entries{};
    ChaosBumpArena m_storage{1024*1024};
  };
}
