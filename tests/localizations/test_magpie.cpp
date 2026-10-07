#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "sc/hash/hash.hpp"
#include "sc/localizations/magpie_string.hpp"
#include "sc/localizations/magpie_translator.hpp"

// Magpie is one table for the whole process, so every test that touches it
// starts by emptying it.
//
// Looking texts up always works. Only the dump depends on the build option
// dump_magpie, so those tests check whichever side this build has.

namespace {

  using sc::Magpie;
  using sc::MagpieKey;
  using sc::MagpieString;

#ifdef DUMP_MAGPIE
  constexpr bool DumpEnabled = true;
#else
  constexpr bool DumpEnabled = false;
#endif

  Magpie &emptyMagpie() {
    Magpie::get()->clear();
    return *Magpie::get();
  }

  void put(std::string_view ns, std::string_view key, std::string_view text) {
    MagpieKey k{ns, key};
    Magpie::get()->insert(k, text, ns, key);
  }

  std::string readFile(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

} // namespace

TEST_CASE("The Dump Option Reaches The Code", "[magpie]") { STATIC_REQUIRE(DumpEnabled == (SC_TEST_EXPECT_DUMP != 0)); }

TEST_CASE("Keys Are Made From Namespace And Name", "[magpie][key]") {
  SECTION("Known at compile time") {
    STATIC_REQUIRE(MagpieKey("menu", "start") == MagpieKey("menu", "start"));
    STATIC_REQUIRE(MagpieKey("menu", "start") != MagpieKey("menu", "quit"));
    STATIC_REQUIRE(MagpieKey("menu", "start") != MagpieKey("hud", "start"));
  }

  SECTION("Texts and their hashes give the same key") {
    REQUIRE(MagpieKey("menu", "start") == MagpieKey(sc::hash("menu"), sc::hash("start")));
  }

  SECTION("Namespace and name are not interchangeable") { REQUIRE(MagpieKey("a", "b") != MagpieKey("b", "a")); }

  SECTION("A name alone is in the namespace 0") {
    REQUIRE(MagpieKey("start") == MagpieKey(0, sc::hash("start")));
    REQUIRE(MagpieKey("start") != MagpieKey("", "start"));
  }

  SECTION("A finished hash is taken as it is") {
    REQUIRE(MagpieKey(1234).key == 1234);
    REQUIRE(MagpieKey().key == 0);
  }
}

TEST_CASE("The Literal Gives The Same Key As The Texts", "[magpie][string]") {
  SECTION("With a namespace") {
    REQUIRE("menu:start"_t.key() == MagpieKey("menu", "start"));
    REQUIRE("menu:start"_t.key() == MagpieString("menu", "start").key());
  }

  SECTION("Without a namespace") { REQUIRE("start"_t.key() == MagpieKey("start")); }

  SECTION("Only the first colon separates") { REQUIRE("a:b:c"_t.key() == MagpieKey("a", "b:c")); }

  SECTION("Known at compile time") {
    constexpr MagpieString text = "menu:start"_t;
    STATIC_REQUIRE(text.key() == MagpieKey("menu", "start"));
  }

  SECTION("From hashes") {
    REQUIRE(MagpieString(sc::hash("menu"), sc::hash("start")).key() == MagpieKey("menu", "start"));
    REQUIRE(MagpieString(MagpieKey("menu", "start")).key() == MagpieKey("menu", "start"));
  }
}

TEST_CASE("Unknown Texts Are Reported As Missing", "[magpie]") {
  auto &magpie = emptyMagpie();

  REQUIRE(magpie.size() == 0);
  REQUIRE(magpie.translate(MagpieKey("menu", "start")) == Magpie::MissingString);
  REQUIRE_FALSE(magpie.find(MagpieKey("menu", "start")).has_value());
  REQUIRE("menu:start"_t.view() == Magpie::MissingString);
  REQUIRE(MagpieString().view() == Magpie::MissingString);
}

TEST_CASE("Inserted Texts Are Found", "[magpie]") {
  auto &magpie = emptyMagpie();

  SECTION("By every way to name the key") {
    put("menu", "start", "Starten");

    REQUIRE(magpie.size() == 1);
    REQUIRE(magpie.translate(MagpieKey("menu", "start")) == "Starten");
    REQUIRE(magpie.find(MagpieKey("menu", "start")) == "Starten");
    REQUIRE("menu:start"_t.view() == "Starten");
    REQUIRE(MagpieString("menu", "start").view() == "Starten");
  }

  SECTION("The text is copied") {
    std::string text = "Starten";
    put("menu", "start", text);
    text = "XXXXXXX";

    REQUIRE("menu:start"_t.view() == "Starten");
  }

  SECTION("The first text for a key stays") {
    put("menu", "start", "Starten");
    put("menu", "start", "Anfangen");

    REQUIRE(magpie.size() == 1);
    REQUIRE("menu:start"_t.view() == "Starten");
  }

  SECTION("The same name in two namespaces") {
    put("menu", "start", "Starten");
    put("hud", "start", "Los");

    REQUIRE(magpie.size() == 2);
    REQUIRE("menu:start"_t.view() == "Starten");
    REQUIRE("hud:start"_t.view() == "Los");
  }

  SECTION("An empty text is not a missing one") {
    put("menu", "empty", "");

    REQUIRE(magpie.find(MagpieKey("menu", "empty")).has_value());
    REQUIRE("menu:empty"_t.view().empty());
  }
}

TEST_CASE("A String Reads Like Its Text", "[magpie][string]") {
  emptyMagpie();
  put("menu", "start", "Starten");
  const MagpieString text = "menu:start"_t;

  const std::string_view view = text;
  REQUIRE(view == "Starten");
  REQUIRE(text.size() == 7);
  REQUIRE(std::string_view(text.data(), text.size()) == "Starten");
  // Usable as a C string.
  REQUIRE(text.data()[text.size()] == '\0');
}

TEST_CASE("A Text Can Be Its Own Key", "[magpie][string]") {
  auto &magpie = emptyMagpie();

  const auto text = MagpieString::fromStr("Hello");

  REQUIRE(text.view() == "Hello");
  REQUIRE(magpie.size() == 1);
  REQUIRE(text.key() == MagpieKey("Hello"));
  REQUIRE("Hello"_t.view() == "Hello");

  SECTION("Twice the same text is one entry") {
    const auto again = MagpieString::fromStr("Hello");

    REQUIRE(magpie.size() == 1);
    REQUIRE(again.key() == text.key());
  }
}

TEST_CASE("The Source Text Stands In For A Missing Translation", "[magpie][string]") {
  emptyMagpie();

  SECTION("Without a translation") {
    const MagpieString text("menu", "start", "Start");

    REQUIRE(text.key() == MagpieKey("menu", "start"));
    REQUIRE(text.view() == "Start");
  }

  SECTION("The translation wins, whenever it arrives") {
    const MagpieString text("menu", "start", "Start");
    put("menu", "start", "Starten");

    REQUIRE(text.view() == "Starten");
  }

  SECTION("Only for the string that knows the source text") {
    const MagpieString text("menu", "start", "Start");

    REQUIRE("menu:start"_t.view() == Magpie::MissingString);
  }
}

TEST_CASE("Stored Strings Are Copies With A Terminator", "[magpie][store]") {
  auto &magpie = emptyMagpie();

  SECTION("A text") {
    std::string source = "Starten";
    const auto stored = magpie.storeStr(source);
    source = "XXXXXXX";

    REQUIRE(stored == "Starten");
    REQUIRE(stored.data()[stored.size()] == '\0');
  }

  SECTION("An empty text") {
    const auto stored = magpie.storeStr("");

    REQUIRE(stored.empty());
    REQUIRE(stored.data() != nullptr);
    REQUIRE(*stored.data() == '\0');
  }

  SECTION("A text of several megabytes") {
    const std::string big(size_t{3} * 1024 * 1024, 'x');
    const auto stored = magpie.storeStr(big);

    REQUIRE(stored == big);
    REQUIRE(magpie.storeStr("after") == "after");
  }

  SECTION("Storing alone adds no entry") {
    (void)magpie.storeStr("Starten");

    REQUIRE(magpie.size() == 0);
  }
}

TEST_CASE("UTF-16 Is Stored As UTF-8", "[magpie][store]") {
  auto &magpie = emptyMagpie();

  SECTION("ASCII") {
    const std::u16string_view source = u"Start";
    const auto stored = magpie.storeStrUTF16(source.data(), source.size());

    REQUIRE(stored == "Start");
    REQUIRE(stored.size() == 5);
    REQUIRE(stored.data()[stored.size()] == '\0');
  }

  SECTION("Umlauts, the euro sign and a surrogate pair") {
    const std::u16string_view source = u"Gr\u00FC\u00DFe \u20AC \U0001F600";
    const auto stored = magpie.storeStrUTF16(source.data(), source.size());

    REQUIRE(stored == "Gr\xC3\xBC\xC3\x9F"
                      "e \xE2\x82\xAC \xF0\x9F\x98\x80");
    REQUIRE(stored.data()[stored.size()] == '\0');
  }

  SECTION("Nothing") {
    const auto stored = magpie.storeStrUTF16(u"", 0);

    REQUIRE(stored.empty());
  }

  SECTION("Invalid UTF-16 gives an empty string") {
    const char16_t source[] = {u'a', 0xD800, u'b'};
    const auto stored = magpie.storeStrUTF16(source, 3);

    REQUIRE(stored.empty());
    REQUIRE(stored.data()[0] == '\0');
  }
}

TEST_CASE("Thread Local Inserts Appear With The Merge", "[magpie][merge]") {
  auto &magpie = emptyMagpie();
  const MagpieKey key("menu", "start");

  SECTION("Not before") {
    Magpie::mt_InsertStored(key, magpie.storeStr("Starten"), "menu", "start");

    REQUIRE(magpie.size() == 0);
    REQUIRE_FALSE(magpie.find(key).has_value());

    magpie.mt_Merge(false);

    REQUIRE(magpie.size() == 1);
    REQUIRE(magpie.translate(key) == "Starten");
  }

  SECTION("Without override the existing text stays") {
    put("menu", "start", "Starten");
    Magpie::mt_InsertStored(key, magpie.storeStr("Anfangen"), "menu", "start");
    magpie.mt_Merge(false);

    REQUIRE(magpie.size() == 1);
    REQUIRE(magpie.translate(key) == "Starten");
  }

  SECTION("With override the new text replaces it") {
    put("menu", "start", "Starten");
    Magpie::mt_InsertStored(key, magpie.storeStr("Anfangen"), "menu", "start");
    magpie.mt_Merge(true);

    REQUIRE(magpie.size() == 1);
    REQUIRE(magpie.translate(key) == "Anfangen");
  }

  SECTION("The text is taken as it is, not copied") {
    const auto stored = magpie.storeStrUTF16(u"Gr\u00FC\u00DFe", 5);
    Magpie::mt_InsertStored(key, stored, "menu", "start");
    magpie.mt_Merge(false);

    REQUIRE(magpie.translate(key).data() == stored.data());
  }

  SECTION("A merge empties the thread's list") {
    Magpie::mt_InsertStored(key, magpie.storeStr("Starten"), "menu", "start");
    magpie.mt_Merge(false);
    magpie.clear();
    magpie.mt_Merge(true);

    REQUIRE(magpie.size() == 0);
  }

  SECTION("Another thread's list is not merged") {
    std::thread([&] {
      Magpie::mt_reserve(1);
      Magpie::mt_InsertStored(key, magpie.storeStr("Starten"), "menu", "start");
    }).join();
    magpie.mt_Merge(false);

    REQUIRE(magpie.size() == 0);
  }
}

TEST_CASE("Many Threads Fill The Table", "[magpie][threads]") {
  auto &magpie = emptyMagpie();
  constexpr size_t Threads = 8;
  constexpr size_t PerThread = 2000;

  const auto name = [](size_t thread, size_t i) { return "t" + std::to_string(thread) + "_" + std::to_string(i); };

  SECTION("Each with its own list") {
    std::vector<std::thread> workers;
    for (size_t t = 0; t < Threads; ++t) {
      workers.emplace_back([&, t] {
        Magpie::mt_reserve(PerThread);
        for (size_t i = 0; i < PerThread; ++i) {
          const std::string text = name(t, i);
          Magpie::mt_InsertStored(MagpieKey("load", text), magpie.storeStr(text), "load", text);
        }
        magpie.mt_Merge(false);
      });
    }
    for (auto &worker : workers) {
      worker.join();
    }

    REQUIRE(magpie.size() == Threads * PerThread);
    for (size_t t = 0; t < Threads; ++t) {
      for (size_t i = 0; i < PerThread; i += 97) {
        const std::string text = name(t, i);
        REQUIRE(magpie.translate(MagpieKey("load", text)) == text);
      }
    }
  }

  SECTION("All inserting the same keys while others read") {
    std::vector<std::thread> workers;
    for (size_t t = 0; t < Threads; ++t) {
      workers.emplace_back([&] {
        for (size_t i = 0; i < PerThread; ++i) {
          const std::string text = name(0, i);
          MagpieKey key("load", text);
          magpie.insert(key, text, "load", text);
          // Either missing or complete, never half written.
          const auto found = magpie.translate(MagpieKey("load", name(0, i / 2)));
          if (found != Magpie::MissingString && found != name(0, i / 2)) {
            FAIL_CHECK("read a damaged text");
          }
        }
      });
    }
    for (auto &worker : workers) {
      worker.join();
    }

    REQUIRE(magpie.size() == PerThread);
    for (size_t i = 0; i < PerThread; ++i) {
      const std::string text = name(0, i);
      REQUIRE(magpie.translate(MagpieKey("load", text)) == text);
    }
  }
}

TEST_CASE("Clear Forgets Everything", "[magpie]") {
  auto &magpie = emptyMagpie();
  put("menu", "start", "Starten");
  (void)MagpieString::fromStr("Hello");
  REQUIRE(magpie.size() == 2);

  magpie.clear();

  REQUIRE(magpie.size() == 0);
  REQUIRE("menu:start"_t.view() == Magpie::MissingString);

  SECTION("And can be filled again") {
    put("menu", "start", "Anfangen");

    REQUIRE("menu:start"_t.view() == "Anfangen");
  }

  SECTION("Again and again") {
    for (int round = 0; round < 200; ++round) {
      const std::string text = "Runde " + std::to_string(round);
      put("menu", "start", text);
      REQUIRE(magpie.storeStr(text) == text);
      REQUIRE("menu:start"_t.view() == text);
      magpie.clear();
    }

    REQUIRE(magpie.size() == 0);
  }

  SECTION("From another thread than the one that stored") {
    std::thread([&] {
      put("menu", "start", "Starten");
      (void)magpie.storeStr(std::string(100'000, 'x'));
    }).join();
    REQUIRE(magpie.size() == 1);

    std::thread([&] { magpie.clear(); }).join();

    REQUIRE(magpie.size() == 0);
    REQUIRE(magpie.storeStr("danach") == "danach");
  }
}

TEST_CASE("The Dump Lists The Texts By Namespace", "[magpie][dump]") {
  auto &magpie = emptyMagpie();
  const auto dir = std::filesystem::temp_directory_path() / "sc_magpie_test";
  const auto file = dir / "nested" / "dump.json";
  std::filesystem::remove_all(dir);

  SECTION("Nothing to dump writes no file") {
    magpie.dumpToFile(file.string());

    REQUIRE_FALSE(std::filesystem::exists(file));
  }

  SECTION("Texts") {
    put("menu", "start", "Starten");
    put("menu", "quit", "Sag \"Tsch\xC3\xBCss\"\n");
    put("hud", "path", "C:\\spiel\t1");
    // Found again under a key that carries no texts.
    REQUIRE(magpie.translate(MagpieKey(sc::hash("hud"), sc::hash("path"))) == "C:\\spiel\t1");

    magpie.dumpToFile(file.string());

    if (DumpEnabled) {
      // The missing directories are created.
      REQUIRE(std::filesystem::exists(file));
      const std::string json = readFile(file);

      const auto hud = json.find(R"("hud": {)");
      const auto path = json.find(R"("path": "C:\\spiel\t1")");
      const auto menu = json.find(R"("menu": {)");
      const auto quit = json.find(R"("quit": "Sag \"Tsch)"
                                  "\xC3\xBC"
                                  R"(ss\"\n")");
      const auto start = json.find(R"("start": "Starten")");

      REQUIRE(hud != std::string::npos);
      REQUIRE(path != std::string::npos);
      REQUIRE(menu != std::string::npos);
      REQUIRE(quit != std::string::npos);
      REQUIRE(start != std::string::npos);
      // Sorted by namespace, then by name.
      REQUIRE(hud < path);
      REQUIRE(path < menu);
      REQUIRE(menu < quit);
      REQUIRE(quit < start);
      REQUIRE(json.front() == '{');
      REQUIRE(json.ends_with("}\n"));
    } else {
      REQUIRE_FALSE(std::filesystem::exists(file));
    }
  }

  std::filesystem::remove_all(dir);
}
