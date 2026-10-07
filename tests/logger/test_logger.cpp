#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "sc/logger/logger.hpp"

namespace {

// All log files of this test program go to a directory of their own, which
// is removed again when the program ends. Set up before main(), so before any
// logger can exist.
struct LogDirectory {
  std::filesystem::path path;

  LogDirectory() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("sc_logger_test_" + std::to_string(stamp));
    sc::setLogDirectory(path);
  }

  ~LogDirectory() {
    // Best effort: where open files cannot be removed, some are left behind.
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

const LogDirectory LOG_DIRECTORY_OWNER;
const std::filesystem::path &LOG_DIRECTORY = LOG_DIRECTORY_OWNER.path;

std::vector<std::string> lines_of(const std::string &module) {
  std::ifstream file(LOG_DIRECTORY / (module + ".log"));
  std::vector<std::string> lines;
  for (std::string line; std::getline(file, line);) {
    lines.push_back(line);
  }
  return lines;
}

bool matches(const std::string &line, const std::string &pattern) {
  return std::regex_match(line, std::regex(pattern));
}

// The start of every line in a log file: the time of day.
const std::string TIME = R"(\[\d\d:\d\d:\d\d\] )";

struct FrameStat {
  static constexpr std::string_view name() { return "Frames"; }
  static std::string str() { return "60"; }
};

struct MemoryStat {
  static constexpr std::string_view name() { return "Memory"; }
  static std::string str() { return "12 MiB"; }
};

} // namespace

TEST_CASE("Logger Writes Every Level To The File", "[logger]") {
  using Log = sc::Logger<"TestLevels">;
  // Nothing on the console, so the test output stays readable.
  Log::init(spdlog::level::off);

  Log::trace("a trace");
  Log::debug("a debug");
  Log::info("an info");
  Log::warn("a warning");
  Log::err("an error");
  Log::critical("a critical");
  Log::log(spdlog::level::info, "through log");
  Log::flush();

  const auto lines = lines_of("TestLevels");
  REQUIRE(lines.size() == 7);
  REQUIRE(matches(lines[0], TIME + R"(\[trace\]: a trace)"));
  REQUIRE(matches(lines[1], TIME + R"(\[debug\]: a debug)"));
  REQUIRE(matches(lines[2], TIME + R"(\[info\]: an info)"));
  REQUIRE(matches(lines[3], TIME + R"(\[warning\]: a warning)"));
  REQUIRE(matches(lines[4], TIME + R"(\[error\]: an error)"));
  REQUIRE(matches(lines[5], TIME + R"(\[critical\]: a critical)"));
  REQUIRE(matches(lines[6], TIME + R"(\[info\]: through log)"));
}

TEST_CASE("Logger Formats Its Arguments", "[logger]") {
  using Log = sc::Logger<"TestFormat">;
  Log::init(spdlog::level::off);

  Log::info("{} + {} = {}", 1, 2, 3);
  Log::info("{} is {:.2f}", std::string("pi"), 3.14159);
  Log::flush();

  const auto lines = lines_of("TestFormat");
  REQUIRE(lines.size() == 2);
  REQUIRE(matches(lines[0], TIME + R"(\[info\]: 1 \+ 2 = 3)"));
  REQUIRE(matches(lines[1], TIME + R"(\[info\]: pi is 3\.14)"));
}

TEST_CASE("Logger Categories", "[logger]") {
  SECTION("A Category Shows Up In The Line") {
    using Log = sc::Logger<"TestCategory", "Net">;
    Log::init(spdlog::level::off);

    Log::info("connected");
    Log::flush();

    const auto lines = lines_of("TestCategory");
    REQUIRE(lines.size() == 1);
    REQUIRE(matches(lines[0], TIME + R"(\[Net\] \[info\]: connected)"));
  }

  SECTION("Categories Of One Module Share Its File") {
    using Reader = sc::Logger<"TestShared", "Reader">;
    using Writer = sc::Logger<"TestShared", "Writer">;
    using Plain = sc::Logger<"TestShared">;
    Reader::init(spdlog::level::off);
    Writer::init(spdlog::level::off);
    Plain::init(spdlog::level::off);

    // The second and third logger must neither empty the file nor write
    // over what is there.
    Reader::info("first");
    Reader::flush();
    Writer::info("second");
    Plain::info("third");
    Reader::info("fourth");
    Reader::flush();

    const auto lines = lines_of("TestShared");
    REQUIRE(lines.size() == 4);
    REQUIRE(matches(lines[0], TIME + R"(\[Reader\] \[info\]: first)"));
    REQUIRE(matches(lines[1], TIME + R"(\[Writer\] \[info\]: second)"));
    REQUIRE(matches(lines[2], TIME + R"(\[info\]: third)"));
    REQUIRE(matches(lines[3], TIME + R"(\[Reader\] \[info\]: fourth)"));
  }
}

TEST_CASE("Logger Timer", "[logger]") {
  using Log = sc::Logger<"TestTimer">;
  Log::init(spdlog::level::off);

  {
    // Format string and argument are temporaries that are gone long before
    // the timer logs.
    auto timer = Log::time(std::string("{} to load ") + "{}",
                           std::string("a save game with a rather long name"));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  {
    auto timer = Log::time(spdlog::level::warn, "{} in total");
  }
  Log::flush();

  const auto lines = lines_of("TestTimer");
  REQUIRE(lines.size() == 2);
  REQUIRE(matches(
      lines[0],
      TIME +
          R"(\[info\]: \d+\.\d\d(ns|µs|ms|s) to load a save game with a rather long name)"));
  REQUIRE(matches(lines[1],
                  TIME + R"(\[warning\]: \d+\.\d\d(ns|µs|ms|s) in total)"));
}

TEST_CASE("Logger Stats", "[logger]") {
  using Log = sc::Logger<"TestStats">;
  Log::init(spdlog::level::off);

  Log::stats<FrameStat, MemoryStat>("after frame {}", 3);
  Log::stats<FrameStat>(spdlog::level::warn, "");
  // Without any stat there is nothing to report.
  Log::stats<>("ignored");
  Log::flush();

  const auto lines = lines_of("TestStats");
  REQUIRE(lines.size() == 2);
  REQUIRE(matches(
      lines[0],
      TIME + R"(\[info\]: after frame 3 -> \[Frames: 60 \| Memory: 12 MiB\])"));
  REQUIRE(matches(lines[1], TIME + R"(\[warning\]: \[Frames: 60\])"));
}

TEST_CASE("Logger Survives A Log File It Cannot Open", "[logger]") {
  // A directory below a regular file can never be created.
  const auto blocker = LOG_DIRECTORY / "blocker";
  std::filesystem::create_directories(LOG_DIRECTORY);
  { std::ofstream file(blocker); }

  sc::setLogDirectory(blocker / "below");
  using Log = sc::Logger<"TestUnwritable">;
  REQUIRE_NOTHROW(Log::init(spdlog::level::off));
  REQUIRE_NOTHROW(Log::warn("still fine"));
  REQUIRE_NOTHROW(Log::flush());
  sc::setLogDirectory(LOG_DIRECTORY);

  REQUIRE(sc::logDirectory() == LOG_DIRECTORY);
}

TEST_CASE("Logger From Several Threads", "[logger][threading]") {
  using Log = sc::Logger<"TestThreads">;
  // Not initialised here on purpose: the threads race for the first use.
  constexpr int thread_count = 8;
  constexpr int per_thread = 500;

  std::vector<std::thread> threads;
  for (int t = 0; t < thread_count; ++t) {
    threads.emplace_back([t]() {
      for (int i = 0; i < per_thread; ++i) {
        Log::debug("thread {} line {}", t, i);
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  Log::flush();

  const auto lines = lines_of("TestThreads");
  REQUIRE(lines.size() == static_cast<size_t>(thread_count) * per_thread);

  // Every line is complete: no two messages ran into each other.
  size_t broken = 0;
  const std::regex whole(TIME + R"(\[debug\]: thread \d line \d+)");
  for (const auto &line : lines) {
    if (!std::regex_match(line, whole)) {
      ++broken;
    }
  }
  REQUIRE(broken == 0);
}
