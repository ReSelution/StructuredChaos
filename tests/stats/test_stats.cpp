#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "sc/stats/counter.hpp"
#include "sc/stats/stats.hpp"
#include "sc/stats/throughput.hpp"
#include "sc/stats/timer.hpp"
#include "sc/stats/units.hpp"

// The trackers (Counter, Throughput), the formatter and the timer always
// work. Only the Stat wrapper and the list of all stats depend on the build
// option enable_chaos_stats, so those tests check whichever side this build
// has.

namespace {

using namespace std::chrono_literals;
using sc::stats::StatsEnabled;

using Metric = sc::stats::ChaosFormatter<sc::stats::MetricUnits>;
using Data = sc::stats::ChaosFormatter<sc::stats::DataUnits>;

using TestCounter = sc::stats::Stat<"Test Counter", sc::stats::Counter<>>;
using TestBytes = sc::stats::Stat<"Test Bytes", sc::stats::Throughput<>>;
using TestGuarded =
    sc::stats::Stat<"Test Guarded",
                    sc::stats::Throughput<sc::stats::MetricUnits>>;
using TestKept = sc::stats::Stat<"Test Kept", sc::stats::Counter<>, false>;

// Stands in for a logger in report_all.
struct CollectingLogger {
  enum class LogLevel { info, warn };

  static inline std::vector<std::pair<LogLevel, std::string>> lines;

  static void log(LogLevel level, std::string_view,
                  std::string_view name, const std::string &text) {
    lines.emplace_back(level, std::string(name) + " -> " + text);
  }
};

std::map<std::string, std::string> report() {
  std::map<std::string, std::string> result;
  sc::stats::report_all_to([&](std::string_view name, const std::string &text) {
    result.emplace(std::string(name), text);
  });
  return result;
}

} // namespace

TEST_CASE("The Build Option Reaches The Code", "[stats]") {
  STATIC_REQUIRE(StatsEnabled == (SC_TEST_EXPECT_STATS != 0));
}

TEST_CASE("Formatter Picks The Unit", "[stats]") {
  SECTION("Counts") {
    REQUIRE(Metric::format(0) == "0.00");
    REQUIRE(Metric::format(999) == "999.00");
    REQUIRE(Metric::format(1000) == "1.00 K");
    REQUIRE(Metric::format(1'500'000) == "1.50 M");
    REQUIRE(Metric::format(2'000'000'000.0) == "2.00 B");
    REQUIRE(Metric::format(-2000) == "-2.00 K");
  }

  SECTION("Bytes") {
    REQUIRE(Data::format(0) == "0.00 B");
    REQUIRE(Data::format(512) == "512.00 B");
    REQUIRE(Data::format(1024) == "1.00 KB");
    REQUIRE(Data::format(1536) == "1.50 KB");
    REQUIRE(Data::format(1024.0 * 1024.0 * 1024.0) == "1.00 GB");
  }

  SECTION("A Value That Rounds Up To The Next Unit Moves There") {
    REQUIRE(Metric::format(999.999) == "1.00 K");
    REQUIRE(Metric::format(999'999.9) == "1.00 M");
    REQUIRE(Data::format(1023.999) == "1.00 KB");
    // Still below the limit after rounding.
    REQUIRE(Metric::format(999.99) == "999.99");
  }

  SECTION("Past The Largest Unit The Number Just Grows") {
    REQUIRE(Metric::format(5e15) == "5000.00 T");
  }

  SECTION("With A Suffix For The Time") {
    REQUIRE(Metric::format(5, "/s") == "5.00 /s");
    REQUIRE(Metric::format(5000, "/s") == "5.00 K/s");
    REQUIRE(Data::format(2048, "/s") == "2.00 KB/s");
  }
}

TEST_CASE("Counter", "[stats]") {
  using Tracker = sc::stats::Counter<>;
  Tracker::Storage storage{"counter"};

  SECTION("Adds Up And Resets") {
    REQUIRE(Tracker::format(storage) == "0.00");
    Tracker::record(storage, 5);
    Tracker::record(storage, 1495);
    REQUIRE(storage.value.load() == 1500);
    REQUIRE(Tracker::format(storage) == "1.50 K");

    Tracker::reset(storage);
    REQUIRE(storage.value.load() == 0);
  }

  SECTION("Counts Down With -1") {
    // The thread pool tracks its queue depth this way.
    Tracker::record(storage, 3);
    Tracker::record(storage, -1);
    Tracker::record(storage, -1);
    REQUIRE(storage.value.load() == 1);
  }

  SECTION("Loses Nothing Across Threads") {
    constexpr int threads = 8;
    constexpr int rounds = 50'000;
    {
      std::vector<std::jthread> workers;
      for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
          for (int i = 0; i < rounds; ++i) {
            Tracker::record(storage, 1);
          }
        });
      }
    }
    REQUIRE(storage.value.load() == uint64_t{threads} * rounds);
  }
}

TEST_CASE("Throughput", "[stats]") {
  using Tracker = sc::stats::Throughput<sc::stats::MetricUnits>;
  Tracker::Storage storage{"throughput"};

  SECTION("Nothing Recorded") {
    REQUIRE(Tracker::format(storage) == "0.00 total | 0.00/s");
    // Stopping what never started changes nothing.
    Tracker::stop(storage);
    REQUIRE(storage.accumulated_ns.load() == 0);
  }

  SECTION("Measures From The First Record To Stop") {
    Tracker::record(storage, 400);
    std::this_thread::sleep_for(20ms);
    Tracker::record(storage, 600);
    Tracker::stop(storage);

    REQUIRE(storage.value.load() == 1000);
    const auto measured = std::chrono::nanoseconds{storage.accumulated_ns.load()};
    REQUIRE(measured >= 20ms);
    REQUIRE(measured < 5s);

    // Stopped: the clock no longer runs, so the text stays the same.
    const std::string first = Tracker::format(storage);
    std::this_thread::sleep_for(5ms);
    REQUIRE(Tracker::format(storage) == first);
    REQUIRE(first.starts_with("1.00 K total | "));
    REQUIRE(first.ends_with("/s"));
    REQUIRE_FALSE(first.ends_with("| 0.00/s"));
  }

  SECTION("Adds Up Several Runs") {
    Tracker::start(storage);
    std::this_thread::sleep_for(10ms);
    Tracker::stop(storage);
    const auto first = storage.accumulated_ns.load();

    Tracker::start(storage);
    std::this_thread::sleep_for(10ms);
    Tracker::stop(storage);

    REQUIRE(std::chrono::nanoseconds{first} >= 10ms);
    REQUIRE(std::chrono::nanoseconds{storage.accumulated_ns.load() - first} >=
            10ms);
  }

  SECTION("Shows A Rate While Still Running") {
    Tracker::record(storage, 1000);
    std::this_thread::sleep_for(5ms);
    REQUIRE(storage.running.load());
    REQUIRE_FALSE(Tracker::format(storage).ends_with("| 0.00/s"));
    // Looking at it does not stop it.
    REQUIRE(storage.running.load());
    REQUIRE(storage.accumulated_ns.load() == 0);
  }

  SECTION("Reset Clears Everything") {
    Tracker::record(storage, 1000);
    std::this_thread::sleep_for(2ms);
    Tracker::stop(storage);
    Tracker::record(storage, 1);

    Tracker::reset(storage);
    REQUIRE(storage.value.load() == 0);
    REQUIRE(storage.accumulated_ns.load() == 0);
    REQUIRE_FALSE(storage.running.load());
    REQUIRE(Tracker::format(storage) == "0.00 total | 0.00/s");
  }

  SECTION("Loses Nothing Across Threads") {
    constexpr int threads = 8;
    constexpr int rounds = 20'000;
    {
      std::vector<std::jthread> workers;
      for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
          for (int i = 0; i < rounds; ++i) {
            Tracker::record(storage, 2);
            if (i % 1000 == 0) {
              (void)Tracker::format(storage);
            }
          }
        });
      }
    }
    Tracker::stop(storage);
    REQUIRE(storage.value.load() == uint64_t{2} * threads * rounds);
    REQUIRE(storage.accumulated_ns.load() > 0);
  }
}

TEST_CASE("Timer", "[stats]") {
  using sc::stats::TimeResult;
  using sc::stats::Timer;
  using sc::stats::Unit;

  SECTION("Reports Once When It Goes Out Of Scope") {
    int calls = 0;
    TimeResult result{};
    {
      Timer timer(
          [&](TimeResult r) {
            ++calls;
            result = r;
          },
          Unit::Milli);
      std::this_thread::sleep_for(10ms);
      REQUIRE(calls == 0);
    }
    REQUIRE(calls == 1);
    REQUIRE(result.unit == Unit::Milli);
    REQUIRE(result.suffix == "ms");
    REQUIRE(result.value >= 10.0);
    REQUIRE(result.value < 5000.0);
  }

  SECTION("Stopping By Hand Reports Once As Well") {
    int calls = 0;
    {
      Timer timer([&](TimeResult) { ++calls; });
      timer.stop();
      REQUIRE(calls == 1);
      timer.stop();
    }
    REQUIRE(calls == 1);
  }

  SECTION("Every Fixed Unit Converts The Same Duration") {
    TimeResult nano{}, micro{}, milli{}, seconds{};
    {
      Timer a([&](TimeResult r) { nano = r; }, Unit::Nano);
      Timer b([&](TimeResult r) { micro = r; }, Unit::Micro);
      Timer c([&](TimeResult r) { milli = r; }, Unit::Milli);
      Timer d([&](TimeResult r) { seconds = r; }, Unit::Seconds);
      std::this_thread::sleep_for(5ms);
    }
    REQUIRE(nano.suffix == "ns");
    REQUIRE(micro.suffix == "µs");
    REQUIRE(milli.suffix == "ms");
    REQUIRE(seconds.suffix == "s");
    REQUIRE(nano.value >= 5e6);
    REQUIRE(micro.value >= 5e3);
    REQUIRE(milli.value >= 5.0);
    REQUIRE(seconds.value >= 5e-3);
    REQUIRE(seconds.value < 5.0);
  }

  SECTION("Auto Picks A Unit That Fits") {
    TimeResult slow{};
    {
      Timer timer([&](TimeResult r) { slow = r; });
      std::this_thread::sleep_for(5ms);
    }
    REQUIRE(slow.unit == Unit::Milli);
    REQUIRE(slow.value >= 5.0);
    REQUIRE(slow.value < 1000.0);

    TimeResult fast{};
    { Timer timer([&](TimeResult r) { fast = r; }); }
    REQUIRE(fast.unit != Unit::Seconds);
    REQUIRE(fast.unit != Unit::Auto);
    REQUIRE(fast.value < 1000.0);
  }

  SECTION("A Moved Timer Reports Once, From Its New Place") {
    int calls = 0;
    {
      Timer first([&](TimeResult) { ++calls; });
      {
        Timer second(std::move(first));
        REQUIRE(calls == 0);
      }
      REQUIRE(calls == 1);
    }
    REQUIRE(calls == 1);
  }

  SECTION("Assigning Over A Timer Finishes It First") {
    std::vector<int> order;
    auto report = [&](int id) {
      return [&order, id](TimeResult) { order.push_back(id); };
    };
    // The same type for both, so one can be assigned to the other.
    using Callback = std::function<void(TimeResult)>;
    {
      Timer<Callback> first(Callback{report(1)});
      Timer<Callback> second(Callback{report(2)});
      first = std::move(second);
      REQUIRE(order == std::vector<int>{1});
    }
    REQUIRE(order == std::vector<int>{1, 2});
  }

  SECTION("Takes A Callback That Lives Elsewhere") {
    int calls = 0;
    auto callback = [&](TimeResult) { ++calls; };
    { Timer timer(callback); }
    { Timer timer(callback, Unit::Micro); }
    REQUIRE(calls == 2);
  }
}

TEST_CASE("Stat Wrapper", "[stats]") {
  TestCounter::reset();
  TestBytes::reset();

  SECTION("Name") {
    REQUIRE(TestCounter::name() == "Test Counter");
    STATIC_REQUIRE(TestBytes::name() == "Test Bytes");
  }

  SECTION("Records Only When Stats Are Enabled") {
    TestCounter::record(1500);
    TestBytes::record(2048);
    TestBytes::stop();

    if (StatsEnabled) {
      REQUIRE(TestCounter::str() == "1.50 K");
      REQUIRE(TestBytes::str().starts_with("2.00 KB total | "));

      TestCounter::reset();
      REQUIRE(TestCounter::str() == "0.00");
    } else {
      REQUIRE(TestCounter::str().empty());
      REQUIRE(TestBytes::str().empty());
    }
  }

  SECTION("ScopeGuard Starts And Stops") {
    TestGuarded::reset();
    {
      sc::stats::ScopeGuard<TestGuarded> guard;
      TestGuarded::record(1000);
      std::this_thread::sleep_for(5ms);
    }
    if (StatsEnabled) {
      const std::string stopped = TestGuarded::str();
      std::this_thread::sleep_for(5ms);
      REQUIRE(TestGuarded::str() == stopped);
      REQUIRE(stopped.starts_with("1.00 K total | "));
      REQUIRE_FALSE(stopped.ends_with("| 0.00/s"));
    } else {
      REQUIRE(TestGuarded::str().empty());
    }
  }
}

TEST_CASE("List Of All Stats", "[stats]") {
  TestCounter::reset();
  TestKept::record(7);

  SECTION("A Stat Is Found By Its Name") {
    const auto found = sc::stats::get_stat("Test Counter");
    if (StatsEnabled) {
      REQUIRE(found.has_value());
      REQUIRE((*found)->internal_name() == "Test Counter");

      TestCounter::record(2000);
      REQUIRE((*found)->internal_str() == "2.00 K");
      (*found)->internal_reset();
      REQUIRE(TestCounter::str() == "0.00");
    } else {
      REQUIRE_FALSE(found.has_value());
    }
    REQUIRE_FALSE(sc::stats::get_stat("No Such Stat").has_value());
  }

  SECTION("The Report Lists Every Stat In Use") {
    TestCounter::record(3);
    const auto all = report();
    if (StatsEnabled) {
      REQUIRE(all.contains("Test Counter"));
      REQUIRE(all.contains("Test Bytes"));
      REQUIRE(all.contains("Test Guarded"));
      REQUIRE(all.contains("Test Kept"));
      REQUIRE(all.at("Test Counter") == "3.00");
    } else {
      REQUIRE(all.empty());
    }
  }

  SECTION("The Report Goes To A Logger") {
    TestCounter::record(3);
    CollectingLogger::lines.clear();
    sc::stats::report_all<CollectingLogger>(CollectingLogger::LogLevel::warn);

    if (StatsEnabled) {
      bool found = false;
      for (const auto &[level, line] : CollectingLogger::lines) {
        REQUIRE(level == CollectingLogger::LogLevel::warn);
        found = found || line == "Test Counter -> 3.00";
      }
      REQUIRE(found);
    } else {
      REQUIRE(CollectingLogger::lines.empty());
    }
  }

  SECTION("Reset All Spares The Stats That Opted Out") {
    TestCounter::record(3);
    const std::string kept = TestKept::str();
    sc::stats::reset_all();

    if (StatsEnabled) {
      REQUIRE(TestCounter::str() == "0.00");
      REQUIRE(TestKept::str() == kept);
      REQUIRE(kept != "0.00");
      // Neither does a direct reset touch it.
      TestKept::reset();
      REQUIRE(TestKept::str() == kept);
    } else {
      REQUIRE(TestKept::str().empty());
    }
  }
}
