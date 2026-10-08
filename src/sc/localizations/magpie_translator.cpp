//
// Created by oleub on 18.04.26.
//

#include "magpie_translator.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

#include "simdutf.h"

namespace sc {
#ifdef DUMP_MAGPIE
  struct NamespaceDeduplicator {
    std::mutex mutex;
    // We store string_view; the characters are owned by Magpie's storage
    ankerl::unordered_dense::set<std::string_view> storage;

    std::string_view get(std::string_view ns, Magpie &magpie) {
      if (ns.empty())
        return "";

      std::lock_guard guard(mutex);
      auto it = storage.find(ns);
      if (it == storage.end()) {
        it = storage.insert(magpie.storeStr(ns)).first;
      }
      return *it;
    }

    void clear() {
      std::lock_guard guard(mutex);
      storage.clear();
    }
  };
  static NamespaceDeduplicator g_nsDeduplicator;
#endif

  void Magpie::clear() noexcept {
    std::unique_lock lock(sh_mtx);
    MagpieInsert::reset();
    MagpieMEM::reset();
    entries.clear();
#ifdef DUMP_MAGPIE
    // Its namespaces live in the storage that is released below.
    g_nsDeduplicator.clear();
#endif
    m_storage.reset();
  }

  void Magpie::dump() noexcept {
#ifdef DUMP_MAGPIE
    std::shared_lock lock(sh_mtx);
    if (entries.empty())
      return;

    // 1. Create a lightweight index of pointers to the map entries
    // Total memory: ~60,000 * 8 bytes = ~480KB. Very cache-friendly.
    std::vector<const std::pair<MagpieKey, std::string_view> *> sortedEntries;
    sortedEntries.reserve(entries.size());
    for (const auto &pair : entries) {
      sortedEntries.push_back(&pair);
    }

    // 2. High-speed Sort
    std::sort(sortedEntries.begin(), sortedEntries.end(), [](auto *a, auto *b) {
      // Namespace comparison (string_view operator<)
      if (a->first.ns_str != b->first.ns_str) {
        return a->first.ns_str < b->first.ns_str;
      }
      // Key comparison within the same namespace
      return a->first.key_str < b->first.key_str;
    });

    // 3. Fast-Buffered Output
    // printf is slow because it parses format strings. For 60k lines,
    // we use a large buffer or raw fwrite for speed.
    std::string_view currentNS = "\1"; // Impossible starting NS

    for (const auto *entry : sortedEntries) {
      const auto &[key, value] = *entry;

      if (key.ns_str != currentNS) {
        currentNS = key.ns_str;
        printf("\n--- Namespace: %.*s ---\n", (int)currentNS.size(), currentNS.data());
      }

      // Fast write
      printf("  %.*s: %.*s\n", (int)key.key_str.size(), key.key_str.data(), (int)value.size(), value.data());
    }
#endif
  }

#ifdef DUMP_MAGPIE
  static void writeEscaped(FILE *f, std::string_view s) {
    for (char c : s) {
      switch (c) {
      case '\"':
        fprintf(f, "\\\"");
        break;
      case '\\':
        fprintf(f, "\\\\");
        break;
      case '\b':
        fprintf(f, "\\b");
        break;
      case '\f':
        fprintf(f, "\\f");
        break;
      case '\n':
        fprintf(f, "\\n");
        break;
      case '\r':
        fprintf(f, "\\r");
        break;
      case '\t':
        fprintf(f, "\\t");
        break;
      default:
        if ((unsigned char)c < 32) {
          fprintf(f, "\\u%04x", c);
        } else {
          fputc(c, f);
        }
      }
    }
  }
#endif
  // NOLINTNEXTLINE ignoring make static when DUMP_MAGPIE is not set
  void Magpie::dumpToFile(std::string_view file) noexcept {
#ifndef DUMP_MAGPIE
    (void)file;
#else
    std::shared_lock lock(sh_mtx);
    if (entries.empty())
      return;

    std::filesystem::path tmpPath(file);
    std::error_code ec;
    // Create directories only if there is a parent path to create
    if (tmpPath.has_parent_path()) {
      std::filesystem::create_directories(tmpPath.parent_path(), ec);
      if (ec) {
        printf("[SAA] [MAGPIE]: Directory error: %s\n", ec.message().c_str());
        return;
      }
    }

    // 1. Sort entries
    std::vector<const std::pair<MagpieKey, std::string_view> *> sorted;
    sorted.reserve(entries.size());
    for (const auto &pair : entries)
      sorted.push_back(&pair);

    std::sort(sorted.begin(), sorted.end(), [](auto *a, auto *b) {
      if (a->first.ns_str != b->first.ns_str)
        return a->first.ns_str < b->first.ns_str;
      return a->first.key_str < b->first.key_str;
    });

    // 2. Open file (using .string().c_str() for platform compatibility)
    FILE *f = fopen(tmpPath.string().c_str(), "wb");
    if (!f)
      return;

    // 3. Start JSON structure
    fputs("{\n", f);

    std::string_view currentNS = "\1";
    bool firstNS = true;

    for (size_t i = 0; i < sorted.size(); ++i) {
      const auto &[key, value] = *sorted[i];

      // Grouping by Namespace
      if (key.ns_str != currentNS) {
        if (!firstNS)
          fputs("\n    },\n", f);

        currentNS = key.ns_str;
        fputs("  \"", f);
        writeEscaped(f, currentNS);
        fputs("\": {", f);

        firstNS = false;
      } else {
        fputs(",", f);
      }

      fputs("\n    \"", f);
      writeEscaped(f, key.key_str);
      fputs("\": \"", f);
      writeEscaped(f, value);
      fprintf(f, "\", \"h\": \"0x%llX\"", (unsigned long long)key.key);
    }

    if (!firstNS)
      fputs("\n    }\n", f);
    fputs("}\n", f);

    fclose(f);
    printf("[SAA] [MAGPIE]: Dumped %zu strings to %s\n", entries.size(), tmpPath.string().c_str());
#endif
  }

  std::string_view Magpie::storeStr(std::string_view str) {
    auto s = m_storage.allocateSpan<char>(str.size() + 1);
    memcpy(s.data(), str.data(), str.size());
    s[str.size()] = '\0';
    return {s.data(), str.size()};
  }

  std::string_view Magpie::storeStrUTF16(const char16_t *start, size_t length) {
    size_t requiredSize = simdutf::utf8_length_from_utf16(start, length);
    char *target = m_storage.allocateSpan<char>(requiredSize + 1).data();
    auto size = simdutf::convert_utf16_to_utf8(start, length, target);
    // simdutf writes nothing and returns 0 for invalid input, e.g. a lone
    // surrogate.
    if (size != requiredSize) [[unlikely]] {
      target[0] = '\0';
      // NOLINTNEXTLINE(bugprone-string-constructor): empty, but still pointing at the stored terminator
      return {target, 0};
    }
    target[requiredSize] = '\0';
    return {target, requiredSize};
  }

  thread_local magpieMAP Magpie::tl_map{};

  void Magpie::insert(MagpieKey &key, std::string_view valueStr, std::string_view ns, std::string_view keyStr) {
    {
      std::shared_lock lock{sh_mtx};
      auto it = entries.find(key);
      if (it != entries.end()) {
#ifdef DUMP_MAGPIE
        key.ns_str = g_nsDeduplicator.get(ns, *this);
        key.key_str = storeStr(keyStr);
#else
        (void)keyStr;
        (void)ns;
#endif
        return;
      }
    }
#ifdef DUMP_MAGPIE
    key.ns_str = g_nsDeduplicator.get(ns, *this);
    key.key_str = storeStr(keyStr);
#else
    (void)keyStr;
    (void)ns;
#endif
    auto s = storeStr(valueStr);
    std::unique_lock lock(sh_mtx);
    if (!entries.contains(key)) {
      entries.emplace(key, s);
    }
  }

  void Magpie::mt_InsertStored(MagpieKey key, std::string_view storedValue, std::string_view ns,
                               std::string_view keyStr) {
#ifdef DUMP_MAGPIE
    key.ns_str = g_nsDeduplicator.get(ns, *this);
    key.key_str = storeStr(keyStr);
#else
    (void)ns;
    (void)keyStr;
#endif

    tl_map.emplace(key, storedValue);
    MagpieMEM::record(storedValue.size());
  }

  void Magpie::mt_Merge(bool override) {
    if (override) {
      std::unique_lock lock(sh_mtx);
      entries.reserve(entries.size() + tl_map.size());
      for (auto [key, value] : tl_map) {
        entries.insert_or_assign(key, value);
      }
    } else {
      std::unique_lock lock(sh_mtx);
      entries.reserve(entries.size() + tl_map.size());
      entries.insert(tl_map.begin(), tl_map.end());
    }
    MagpieInsert::record(tl_map.size());
    tl_map.clear();
  }
} // namespace sc
