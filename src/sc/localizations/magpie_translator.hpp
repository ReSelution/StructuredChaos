#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string_view>

#include <ankerl/unordered_dense.h>

#include "sc/config/config.h"
#include "sc/hash/hash.hpp"
#include "sc/memory/heap.hpp"
#include "sc/stats/stats.hpp"
#include "sc/stats/throughput.hpp"

namespace sc {
  using MagpieInsert = sc::stats::Stat<"Magpie Throughput", sc::stats::Throughput<sc::stats::MetricUnits>>;
  using MagpieMemory = sc::stats::Stat<"Magpie Memory", sc::stats::Throughput<>>;

  struct MagpieKey {
    uint64_t key = 0;
#ifdef DUMP_MAGPIE
    std::string_view ns_str{};
    std::string_view key_str{};
#endif

    MagpieKey() = default;

    constexpr explicit MagpieKey(uint64_t key) : key(key) {}

    constexpr MagpieKey(uint64_t ns, uint64_t k) : key(sc::hash(ns, k)) {}

    constexpr MagpieKey(std::string_view ns, std::string_view key) : MagpieKey(sc::hash(ns), sc::hash(key)) {}

    constexpr explicit MagpieKey(std::string_view key) : MagpieKey(0, sc::hash(key)) {}

    constexpr bool operator==(const MagpieKey &other) const noexcept { return key == other.key; }
  };

  struct MagpieKeyHasher {
    using is_avalanching = void;

    size_t operator()(const MagpieKey &k) const noexcept { return k.key; }
  };

  using MagpieMap = ankerl::unordered_dense::map<MagpieKey, std::string_view, MagpieKeyHasher>;

  // What mergeLocal() does with a key that is in the table already.
  enum class MergeMode { KeepExisting, Override };

  // The table of all texts of the process, by key.
  class Magpie {
  public:
    static Magpie &get() {
      static Magpie magpie{};
      return magpie;
    }

    static constexpr std::string_view MISSING_STRING = "<Magpie missing String>";

    std::optional<std::string_view> find(const MagpieKey key) const {
      std::shared_lock lock(m_mutex);
      if (auto it = m_entries.find(key); it != m_entries.end()) [[likely]] {
        return it->second;
      }
      return std::nullopt;
    }

    std::string_view translate(const MagpieKey key) const { return find(key).value_or(MISSING_STRING); }

    // Copies the text into the storage and adds it, unless the key has one
    // already. ns and keyStr are only kept with dump_magpie.
    void insert(MagpieKey key, std::string_view valueStr, std::string_view ns, std::string_view keyStr);

    // For loading many texts from several threads: every thread fills a map of
    // its own, which mergeLocal() then moves into the table in one go.
    void reserveLocal(size_t size);

    // Keeps the view as it is instead of copying the text, so that a text can
    // be converted straight into the storage (storeStr, storeStrUTF16) without
    // a temporary. The text has to live until the next clear().
    void insertStoredLocal(MagpieKey key, std::string_view storedValue, std::string_view ns, std::string_view keyStr);

    // Moves the texts of this thread's map into the table.
    void mergeLocal(MergeMode mode);

    [[nodiscard]] size_t size() const {
      std::shared_lock lock(m_mutex);
      return m_entries.size();
    }

    // Frees all stored texts. Must not run while another thread stores one.
    void clear();
    // Print or write every text, sorted by namespace and key. Only with
    // dump_magpie, otherwise they do nothing.
    void dump() const;
    void dumpToFile(std::string_view file) const;

    // Copies a text into the storage, which owns it until clear(). Safe from
    // several threads at the same time without the table's lock, because the
    // heap allocates thread-safely.
    std::string_view storeStr(std::string_view str);
    std::string_view storeStrUTF16(const char16_t *start, size_t length);

  private:
    // Sets the texts of the key that dump() shows, with dump_magpie.
    MagpieKey withDebugStrings(MagpieKey key, std::string_view ns, std::string_view keyStr);

    static inline thread_local MagpieMap s_localMap{};
    alignas(64) mutable std::shared_mutex m_mutex;
    MagpieMap m_entries;
    // Owns every stored text.
    Heap m_storage;
  };
} // namespace sc
