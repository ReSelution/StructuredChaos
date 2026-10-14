#include "magpie_translator.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <simdutf.h>

#include "sc/logger/logger.hpp"

namespace sc {
  namespace {
    using MagpieLog = sc::Logger<"Magpie">;

#ifdef DUMP_MAGPIE
    struct NamespaceDeduplicator {
      std::mutex mutex;
      // The characters are owned by Magpie's storage.
      ankerl::unordered_dense::set<std::string_view> storage;

      std::string_view get(std::string_view ns, Magpie &magpie) {
        if (ns.empty()) {
          return "";
        }

        std::scoped_lock lock(mutex);
        auto it = storage.find(ns);
        if (it == storage.end()) {
          it = storage.insert(magpie.storeStr(ns)).first;
        }
        return *it;
      }

      void clear() {
        std::scoped_lock lock(mutex);
        storage.clear();
      }
    };

    NamespaceDeduplicator &namespaces() {
      static NamespaceDeduplicator deduplicator;
      return deduplicator;
    }

    struct DumpEntry {
      std::string_view ns;
      std::string_view key;
      std::string_view value;
      uint64_t hash;
    };

    // Every entry, sorted by namespace and then key. Copied out under the lock
    // so that the caller can write them without holding it; the texts stay
    // valid until the next clear().
    std::vector<DumpEntry> sortedEntries(const MagpieMap &entries) {
      std::vector<DumpEntry> sorted;
      sorted.reserve(entries.size());
      for (const auto &[key, value]: entries) {
        sorted.push_back({.ns = key.ns_str, .key = key.key_str, .value = value, .hash = key.key});
      }
      std::ranges::sort(
          sorted, [](const DumpEntry &a, const DumpEntry &b) { return std::tie(a.ns, a.key) < std::tie(b.ns, b.key); });
      return sorted;
    }

    void writeEscaped(std::ostream &out, std::string_view s) {
      for (const char c: s) {
        switch (c) {
          case '\"':
            out << "\\\"";
            break;
          case '\\':
            out << "\\\\";
            break;
          case '\b':
            out << "\\b";
            break;
          case '\f':
            out << "\\f";
            break;
          case '\n':
            out << "\\n";
            break;
          case '\r':
            out << "\\r";
            break;
          case '\t':
            out << "\\t";
            break;
          default:
            if (static_cast<unsigned char>(c) < 32) {
              std::array<char, 8> escaped{};
              std::snprintf(escaped.data(), escaped.size(), "\\u%04x", static_cast<unsigned>(c));
              out << escaped.data();
            } else {
              out << c;
            }
        }
      }
    }
#endif
  } // namespace

  void Magpie::clear() {
    std::unique_lock lock(m_mutex);
    MagpieInsert::reset();
    MagpieMemory::reset();
    m_entries.clear();
#ifdef DUMP_MAGPIE
    // Its namespaces live in the storage that is released below.
    namespaces().clear();
#endif
    m_storage.reset();
  }

  void Magpie::dump() const {
#ifdef DUMP_MAGPIE
    std::vector<DumpEntry> sorted;
    {
      std::shared_lock lock(m_mutex);
      sorted = sortedEntries(m_entries);
    }

    std::optional<std::string_view> currentNs;
    for (const DumpEntry &entry: sorted) {
      if (entry.ns != currentNs) {
        currentNs = entry.ns;
        MagpieLog::info("--- Namespace: {} ---", entry.ns);
      }
      MagpieLog::info("  {}: {}", entry.key, entry.value);
    }
#endif
  }

  // NOLINTNEXTLINE(readability-convert-member-functions-to-static): uses members with dump_magpie
  void Magpie::dumpToFile(std::string_view file) const {
#ifndef DUMP_MAGPIE
    (void) file;
#else
    std::vector<DumpEntry> sorted;
    {
      std::shared_lock lock(m_mutex);
      sorted = sortedEntries(m_entries);
    }
    if (sorted.empty()) {
      return;
    }

    const std::filesystem::path path(file);
    if (path.has_parent_path()) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      if (ec) {
        MagpieLog::err("Cannot create the directory of {}: {}", path.string(), ec.message());
        return;
      }
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) {
      MagpieLog::err("Cannot open {} for writing", path.string());
      return;
    }

    out << "{\n";
    std::optional<std::string_view> currentNs;
    for (const DumpEntry &entry: sorted) {
      if (entry.ns != currentNs) {
        if (currentNs) {
          out << "\n    },\n";
        }
        currentNs = entry.ns;
        out << "  \"";
        writeEscaped(out, entry.ns);
        out << "\": {";
      } else {
        out << ',';
      }

      out << "\n    \"";
      writeEscaped(out, entry.key);
      out << "\": \"";
      writeEscaped(out, entry.value);
      out << std::format(R"(", "h": "0x{:X}")", entry.hash);
    }
    out << "\n    }\n}\n";

    out.close();
    if (!out) {
      MagpieLog::err("Writing {} failed", path.string());
      return;
    }
    MagpieLog::info("Dumped {} strings to {}", sorted.size(), path.string());
#endif
  }

  std::string_view Magpie::storeStr(std::string_view str) {
    auto s = m_storage.allocateSpan<char>(str.size() + 1);
    std::memcpy(s.data(), str.data(), str.size());
    s[str.size()] = '\0';
    return {s.data(), str.size()};
  }

  std::string_view Magpie::storeStrUTF16(const char16_t *start, size_t length) {
    const size_t requiredSize = simdutf::utf8_length_from_utf16(start, length);
    char *target = m_storage.allocateSpan<char>(requiredSize + 1).data();
    const size_t size = simdutf::convert_utf16_to_utf8(start, length, target);
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

  // NOLINTNEXTLINE(readability-convert-member-functions-to-static): uses members with dump_magpie
  MagpieKey Magpie::withDebugStrings(MagpieKey key, [[maybe_unused]] std::string_view ns,
                                     [[maybe_unused]] std::string_view keyStr) {
#ifdef DUMP_MAGPIE
    key.ns_str = namespaces().get(ns, *this);
    key.key_str = storeStr(keyStr);
#endif
    return key;
  }

  void Magpie::insert(MagpieKey key, std::string_view valueStr, std::string_view ns, std::string_view keyStr) {
    {
      std::shared_lock lock(m_mutex);
      if (m_entries.contains(key)) {
        return;
      }
    }
    // Stored before the exclusive lock is taken. If another thread inserts the
    // same key in the meantime, this copy stays unused until clear().
    key = withDebugStrings(key, ns, keyStr);
    const std::string_view stored = storeStr(valueStr);
    std::unique_lock lock(m_mutex);
    m_entries.try_emplace(key, stored);
  }

  // NOLINTNEXTLINE(readability-convert-member-functions-to-static): one API for all of the local map
  void Magpie::reserveLocal(size_t size) { s_localMap.reserve(size); }

  void Magpie::insertStoredLocal(MagpieKey key, std::string_view storedValue, std::string_view ns,
                                 std::string_view keyStr) {
    s_localMap.emplace(withDebugStrings(key, ns, keyStr), storedValue);
    MagpieMemory::record(storedValue.size());
  }

  void Magpie::mergeLocal(MergeMode mode) {
    {
      std::unique_lock lock(m_mutex);
      m_entries.reserve(m_entries.size() + s_localMap.size());
      if (mode == MergeMode::Override) {
        for (const auto &[key, value]: s_localMap) {
          m_entries.insert_or_assign(key, value);
        }
      } else {
        m_entries.insert(s_localMap.begin(), s_localMap.end());
      }
    }
    MagpieInsert::record(s_localMap.size());
    s_localMap.clear();
  }
} // namespace sc
