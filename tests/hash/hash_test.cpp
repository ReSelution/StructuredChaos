//
// Created by oleub on 11.04.26.
//


#include <filesystem>
#include <fstream>
#include "threading/chaos_threading.hpp"
#include "city.h"
#include "hash/hash.hpp"

LOG_ALIAS(HashLog, "Chaos", "Hash");
DEFINE_CHAOS_CORE_STAT(HashThroughput, "Hash throughput", SC::ChaosThroughput<SC::MetricUnits>);

void setWorkingDirectory(const char* argv0) {
    namespace fs = std::filesystem;
    fs::path exePath = std::filesystem::canonical(argv0);
    fs::path projectRoot = exePath.parent_path().parent_path().parent_path();
    HashLog::info("{}", projectRoot.string());
    fs::current_path(projectRoot);
}

struct StringBlock {
    std::vector<char> data;
    std::vector<std::string_view> views;
};

StringBlock loadStringsPacked(const std::string& path) {
    std::ifstream file(path);
    StringBlock block;

    if (!file.is_open()) {
        HashLog::err("Failed to open {}", path);
        return block;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        const size_t offset = block.data.size();

        block.data.insert(block.data.end(), line.begin(), line.end());

        block.views.emplace_back(
                block.data.data() + offset,
                line.size()
        );
    }

    HashLog::info("Loaded {} strings (packed)", block.views.size());
    return block;
}

uint64_t hashPath(const std::string_view path) {

    char16_t buffer[512];
    const size_t len = std::min<size_t>(path.length(), 512);

    for (size_t i = 0; i < len; ++i) {
        const auto uc = static_cast<char>(path[i]);
        char lc = (uc >= 'A' && uc <= 'Z') ? static_cast<char>(uc + 32) : uc;
        buffer[i] = static_cast<char16_t>(lc);
    }

    return CityHash64(reinterpret_cast<const char *>(buffer), len * sizeof(char16_t));
}

uint64_t hashStr(const std::string_view str) {
    constexpr size_t MAX_LEN = 512;

    char16_t buffer[MAX_LEN];
    const size_t len = std::min(str.size(), MAX_LEN);

    for (size_t i = 0; i < len; ++i) {
        const auto uc = static_cast<unsigned char>(str[i]);
        const char lc = static_cast<char>(
                uc + (((uc - 'A' < 26u) ? 1u : 0u) << 5)
        );

        buffer[i] = static_cast<char16_t>(lc);
    }

    const uint64_t hash =
            CityHash64(reinterpret_cast<const char *>(buffer),
                       len * sizeof(char16_t));

    // optional safety check (falls reservierter Wert existiert)
    assert(hash != ~0ull && "Hash collision with invalid sentinel");

    return hash;
}

uint64_t oldHash(const std::string_view name){
    char16_t buffer[512];
    const size_t len = std::min(name.length(), (size_t) 511);

    for (size_t i = 0; i < len; ++i) {
        // Lowercase + UTF16-Promoting in einem Schritt
        const auto uc = static_cast<unsigned char>(name[i]);
        char lc = static_cast<char>(uc + (((uc - 'A' < 26u) ? 1u : 0u) << 5));
        buffer[i] = static_cast<char16_t>(lc);
    }

    uint64_t hash = CityHash64(reinterpret_cast<const char *>(buffer), len * sizeof(char16_t));
    assert(hash != ~0uL && "Package name hash collision with InvalidId");
    return hash;
}


template <typename HashFn>
std::vector<uint64_t> runHashStressTest(const StringBlock& block, HashFn&& hashFn, std::string_view name, int iterations = 100) {
    auto &strings = block.views;
    const size_t stringsPerIter = strings.size();
    const size_t totalOps = stringsPerIter * iterations;

    if (stringsPerIter == 0) return {};


    std::vector<uint64_t> results(stringsPerIter);
    uint64_t dummySum = 0;
    HashLog::info("Benchmarking {}: {} iterations ({} total hashes)...", name, iterations, totalOps);
    HashThroughput::reset();
    {
        auto t = HashLog::time("{1}: {0} for {2} hashes", name, totalOps);

        for (int i = 0; i < iterations; ++i) {
            for (size_t j = 0; j < stringsPerIter; ++j) {
                uint64_t h = hashFn(strings[j]);
                results[j] = h;
                dummySum ^= h;
            }

            HashThroughput::record(stringsPerIter);
        }
    }
    if (dummySum == 0x1) HashLog::info("Sum: {:x}", dummySum);
    HashLog::stats<HashThroughput>("{}", name);
    return  results;
}

void verifyHashes(const std::vector<uint64_t>& reference, const std::vector<uint64_t>& current, std::string_view name) {
    if (reference.size() != current.size()) {
        HashLog::err("Verification FAILED for {}: Size mismatch! ({} vs {})\n", name, reference.size(), current.size());
        return;
    }

    size_t errors = 0;
    for (size_t i = 0; i < reference.size(); ++i) {
        if (reference[i] != current[i]) {
            if (errors < 5) { // Nur die ersten 5 Fehler zeigen, um den Log nicht zu fluten
                HashLog::err("Hash mismatch at index {}: Ref {:x} != Current {:x}", i, reference[i], current[i]);
            }
            errors++;
        }
    }

    if (errors == 0) {
        HashLog::info("Verification PASSED for {}: All {} hashes match.\n", name, current.size());
    } else {
        HashLog::err("Verification FAILED for {}: {} mismatches found!\n", name, errors);
    }
}

void testFileHash(){
    auto block = loadStringsPacked("tests/hash/files.txt");
    HashLog ::info("Starting Hash Test with {} strings", block.views.size());
    auto refHashes = runHashStressTest(block, oldHash, "Old Hash (Ref)", 500);

    auto pathHashes = runHashStressTest(block, SC::hash::cityHash64_ue, "City SIMD Lower", 500);
    verifyHashes(refHashes, pathHashes, "Hash Path");


    auto xxHash = runHashStressTest(block, SC::hash::xxhash_lowercase, "xxHash", 500);


}

void testSSHash(){
    auto block = loadStringsPacked("tests/hash/smallString.txt");
    HashLog ::info("Starting Hash Test with {} strings", block.views.size());
    runHashStressTest(block, SC::hash::xxhash, "xxHash", 500);

}

int main(int argc, char** argv){
    if (argc > 0) {
        setWorkingDirectory(argv[0]);
    }

    SC::ChaosThreading::init();
    testFileHash();
    testSSHash();
}