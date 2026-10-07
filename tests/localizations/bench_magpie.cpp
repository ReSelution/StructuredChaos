#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "sc/localizations/magpie_translator.hpp"

// Hidden by the [!benchmark] tag, so that a plain test run stays fast. Run with
//   meson test --benchmark -C <builddir>
// or directly with
//   test_magpie "[!benchmark]"
// Use an optimized build. Every benchmark fills the table with texts and
// empties it again, so one run is a whole load of a language.

namespace {

  using sc::Magpie;
  using sc::MagpieKey;

  struct Text {
    std::string key;
    std::string value;
    std::u16string value16;
  };

  // Texts of 8 to 71 characters, like the labels and sentences of a game.
  std::vector<Text> make_texts(size_t count, size_t thread) {
    std::vector<Text> texts;
    texts.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      std::string key = "t" + std::to_string(thread) + "_" + std::to_string(i);
      std::string value = key + std::string(8 + ((i * 7) % 64) - (key.size() % 8), static_cast<char>('a' + (i % 26)));
      std::u16string value16(value.begin(), value.end());
      texts.push_back({.key = std::move(key), .value = std::move(value), .value16 = std::move(value16)});
    }
    return texts;
  }

} // namespace

TEST_CASE("Benchmark: storing texts", "[!benchmark][magpie]") {
  constexpr size_t COUNT = 10'000;
  auto &magpie = *Magpie::get();
  const auto texts = make_texts(COUNT, 0);

  BENCHMARK("storeStr: 10000 texts, then clear") {
    size_t sum = 0;
    for (const Text &text : texts) {
      sum += magpie.storeStr(text.value).size();
    }
    magpie.clear();
    return sum;
  };

  BENCHMARK("storeStrUTF16: 10000 texts, then clear") {
    size_t sum = 0;
    for (const Text &text : texts) {
      sum += magpie.storeStrUTF16(text.value16.data(), text.value16.size()).size();
    }
    magpie.clear();
    return sum;
  };

  BENCHMARK("insert: 10000 texts, then clear") {
    for (const Text &text : texts) {
      MagpieKey key("bench", text.key);
      magpie.insert(key, text.value, "bench", text.key);
    }
    const size_t size = magpie.size();
    magpie.clear();
    return size;
  };
}

TEST_CASE("Benchmark: filling the table from several threads", "[!benchmark][magpie]") {
  constexpr size_t THREADS = 8;
  constexpr size_t PER_THREAD = 5'000;
  auto &magpie = *Magpie::get();

  std::vector<std::vector<Text>> texts;
  for (size_t t = 0; t < THREADS; ++t) {
    texts.push_back(make_texts(PER_THREAD, t));
  }

  BENCHMARK("mt_InsertStored + mt_Merge: 8 threads x 5000, then clear") {
    {
      std::vector<std::jthread> workers;
      for (size_t t = 0; t < THREADS; ++t) {
        workers.emplace_back([&, t] {
          Magpie::mt_reserve(PER_THREAD);
          for (const Text &text : texts[t]) {
            magpie.mt_InsertStored(MagpieKey("bench", text.key), magpie.storeStr(text.value), "bench", text.key);
          }
          magpie.mt_Merge(false);
        });
      }
    }
    const size_t size = magpie.size();
    magpie.clear();
    return size;
  };

  BENCHMARK("insert: 8 threads x 5000, then clear") {
    {
      std::vector<std::jthread> workers;
      for (size_t t = 0; t < THREADS; ++t) {
        workers.emplace_back([&, t] {
          for (const Text &text : texts[t]) {
            MagpieKey key("bench", text.key);
            magpie.insert(key, text.value, "bench", text.key);
          }
        });
      }
    }
    const size_t size = magpie.size();
    magpie.clear();
    return size;
  };
}
