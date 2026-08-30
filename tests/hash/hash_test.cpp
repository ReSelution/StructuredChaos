#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// Importiere deine Module
import sc.logger;

import sc.hash;
import sc.stats;

using Log            = sc::Logger<"Hash">;
using HashThroughout = sc::stats::Stat<"Hash throughput", sc::stats::Throughput<sc::stats::MetricUnits>>;

void setWorkingDirectory(const char *argv0) {
  namespace fs         = std::filesystem;
  fs::path exePath     = std::filesystem::canonical(argv0);
  fs::path projectRoot = exePath.parent_path().parent_path().parent_path();
  Log::info("Project Root Path: {}", projectRoot.string());
  fs::current_path(projectRoot);
}

struct StringBlock {
  std::vector<char> data;
  std::vector<std::string_view> views;
};

StringBlock loadStringsPacked(const std::string &path) {
  std::ifstream file(path);
  StringBlock block;

  if (!file.is_open()) {
    Log::err("Failed to open {}", path);
    return block;
  }

  std::string line;
  std::vector<size_t> offsets;
  std::vector<size_t> lengths;

  while (std::getline(file, line)) {
    if (line.empty())
      continue;

    offsets.push_back(block.data.size());
    lengths.push_back(line.size());

    block.data.insert(block.data.end(), line.begin(), line.end());
  }

  // Zweiter Schritt: Jetzt, wo block.data stabil im Speicher liegt
  // und sich nie wieder bewegt, weisen wir die string_views zu.
  block.views.reserve(offsets.size());
  for (size_t i = 0; i < offsets.size(); ++i) {
    block.views.emplace_back(block.data.data() + offsets[i], lengths[i]);
  }

  Log::info("Loaded {} strings (packed without invalidation)", block.views.size());
  return block;
}

constexpr uint64_t hash_lowercaseOld(std::string_view str) {
  constexpr size_t MAX_STR_LEN = 512;
  uint8_t buffer[MAX_STR_LEN];
  const size_t len = std::min<size_t>(str.length(), MAX_STR_LEN);

  for (size_t i = 0; i < len; ++i) {
    const auto c = static_cast<uint8_t>(str[i]);
    buffer[i]    = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c);
  }
  return sc::hash(buffer, len);
}

template<typename HashFn>
std::vector<uint64_t> runHashStressTest(const StringBlock &block, HashFn &&hashFn, std::string_view name,
                                        int iterations = 100) {
  auto &strings               = block.views;
  const size_t stringsPerIter = strings.size();
  const size_t totalOps       = stringsPerIter * iterations;

  if (stringsPerIter == 0)
    return {};

  std::vector<uint64_t> results(stringsPerIter);
  uint64_t dummySum = 0;
  Log::info("Benchmarking {}: {} iterations ({} total hashes)...", name, iterations, totalOps);

  HashThroughout::reset();
  {

    for (int i = 0; i < iterations; ++i) {
      for (size_t j = 0; j < stringsPerIter; ++j) {
        uint64_t h = hashFn(strings[j]);
        results[j] = h;
        dummySum ^= h;
      }
      HashThroughout::record(stringsPerIter);
    }
  }

  if (dummySum == 0x1)
    Log::info("Sum: {:x}", dummySum);

  Log::stats<HashThroughout>("{}", name);
  return results;
}

void verifyHashes(const std::vector<uint64_t> &reference, const std::vector<uint64_t> &current, std::string_view name) {
  if (reference.size() != current.size()) {
    Log::err("Verification FAILED for {}: Size mismatch! ({} vs {})\n", name, reference.size(), current.size());
    return;
  }

  size_t errors = 0;
  for (size_t i = 0; i < reference.size(); ++i) {
    if (reference[i] != current[i]) {
      if (errors < 5) {
        Log::err("Hash mismatch at index {}: Ref {:x} != Current {:x}", i, reference[i], current[i]);
      }
      errors++;
    }
  }

  if (errors == 0) {
    Log::info("Verification PASSED for {}: All {} hashes match.\n", name, current.size());
  } else {
    Log::err("Verification FAILED for {}: {} mismatches found!\n", name, errors);
  }
}
constexpr std::string_view ConstStr = "Engine/Config/Base.ini";
void testFileHash() {
  auto block = loadStringsPacked("tests/hash/files.txt");
  if (block.views.empty())
    return;

  Log::info("Starting Hash Test with {} strings", block.views.size());

  auto rapid    = runHashStressTest(block, sc::hash_lowercase, "rapidLower (SWAR)", 500);
  auto rapidold = runHashStressTest(block, hash_lowercaseOld, "rapidLowerOld (Byte)", 500);

  verifyHashes(rapid, rapidold, "rapid SWAR vs Old");

  constexpr auto hash = sc::hash(ConstStr);
  auto runtimeHash    = sc::hash(block.views[0]);
  if (hash == runtimeHash) {
    Log::info("CompileTime Hash Matches Runtime Hash");
  } else {
    Log::err("Runtime mismatches CompileTime Hash");
  }
}

void testSSHash() {
  auto block = loadStringsPacked("tests/hash/smallString.txt");
  if (block.views.empty())
    return;

  Log::info("Starting Hash Test with {} strings", block.views.size());
  runHashStressTest(block, [](auto &&s) { return sc::hash(s); }, "rapid-generic", 500);
}

int main(int argc, char **argv) {
  if (argc > 0) {
    setWorkingDirectory(argv[0]);
  }

  testFileHash();
  testSSHash();

  return 0;
}
