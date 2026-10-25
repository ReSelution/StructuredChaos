#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include "sc/threading/threading.hpp"

#ifdef __linux__
#include <sched.h>
#endif

namespace {
  using namespace std::chrono_literals;

  // Spins until the flag is set; a test that hangs runs into the timeout of
  // meson instead.
  void waitFor(const std::atomic<bool> &flag) {
    while (!flag.load()) {
      std::this_thread::yield();
    }
  }
} // namespace

TEST_CASE("Background spawn", "[threading][background]") {
  sc::threading::init();

  SECTION("gives the result") {
    auto task = sc::threading::spawn([] { return 42; });
    REQUIRE(task.get() == 42);
    REQUIRE_FALSE(task.valid());
  }

  SECTION("void and move-only results") {
    std::atomic<bool> ran{false};
    auto done = sc::threading::spawn([&] { ran = true; });
    done.wait();
    REQUIRE(done.ready());
    done.get();
    REQUIRE(ran);

    auto boxed = sc::threading::spawn([] { return std::make_unique<int>(7); });
    REQUIRE(*boxed.get() == 7);
  }

  SECTION("passes an exception on through get") {
    auto task = sc::threading::spawn([]() -> int { throw std::runtime_error("load failed"); });
    REQUIRE_THROWS_AS(task.get(), std::runtime_error);
  }

  SECTION("runs on a lane thread, not on the pool") {
    auto task =
        sc::threading::spawn([] { return std::pair{sc::threading::onBackgroundThread(), sc::threading::threadId()}; });
    const auto [background, id] = task.get();
    REQUIRE(background);
    REQUIRE(id == sc::threading::NO_THREAD_ID);
    REQUIRE_FALSE(sc::threading::onBackgroundThread());
  }

#ifdef __linux__
  SECTION("runs at SCHED_IDLE") {
    REQUIRE(sc::threading::spawn([] { return sched_getscheduler(0); }).get() == SCHED_IDLE);
  }
#endif

  SECTION("cancel stops a running task through its token") {
    std::atomic<bool> started{false};
    auto task = sc::threading::spawn([&](const std::stop_token &stop) {
      started = true;
      size_t rounds = 0;
      while (!stop.stop_requested()) {
        ++rounds;
        std::this_thread::yield();
      }
      return rounds;
    });
    waitFor(started);
    task.cancel();
    (void) task.get();
  }

  SECTION("a task cancelled before it started never runs") {
    // Every lane thread blocks, so the next one waits in the queue.
    std::atomic<bool> release{false};
    std::vector<sc::threading::BackgroundTask<void>> blockers;
    for (uint32_t i = 0; i < sc::threading::defaultBackgroundCount(); ++i) {
      blockers.push_back(sc::threading::spawn([&] { waitFor(release); }));
    }
    std::atomic<bool> ran{false};
    auto task = sc::threading::spawn([&] { ran = true; });
    task.cancel();
    release = true;
    REQUIRE_THROWS_AS(task.get(), std::future_error);
    for (auto &blocker: blockers) {
      blocker.get();
    }
    REQUIRE_FALSE(ran);
  }

  SECTION("destroying the handle cancels") {
    std::atomic<bool> stopped{false};
    std::atomic<bool> started{false};
    {
      auto task = sc::threading::spawn([&](const std::stop_token &stop) {
        started = true;
        while (!stop.stop_requested()) {
          std::this_thread::yield();
        }
        stopped = true;
      });
      waitFor(started);
    }
    waitFor(stopped);
  }

  SECTION("a parallelFor of the lane gets helpers of an idle pool") {
    auto task = sc::threading::spawn([] {
      std::atomic<size_t> foreign{0};
      const auto self = std::this_thread::get_id();
      sc::threading::parallelFor(1000, 1, [&](size_t first, size_t) {
        if (std::this_thread::get_id() != self) {
          foreign.fetch_add(1);
        } else if (first == 0) {
          // Leaves the rest to the workers for a while.
          const auto deadline = std::chrono::steady_clock::now() + 5s;
          while (foreign.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
          }
        }
      });
      return foreign.load();
    });
    REQUIRE(task.get() > 0);
  }

  SECTION("a parallelFor of the lane gets half the workers by default") {
    const size_t half = (sc::threading::threadSlots() - 1) / 2;
    auto task = sc::threading::spawn([] {
      std::mutex mutex;
      std::set<std::thread::id> threads;
      sc::threading::parallelFor(200, 1, [&](size_t, size_t) {
        {
          const std::scoped_lock lock(mutex);
          threads.insert(std::this_thread::get_id());
        }
        std::this_thread::sleep_for(1ms);
      });
      return threads.size();
    });
    // The lane thread and at most half of the workers.
    REQUIRE(task.get() <= half + 1);
  }

  SECTION("the workers in a loop of the lane leave it for the frame") {
    const size_t workers = sc::threading::threadSlots() - 1;
    constexpr size_t LANE_CHUNKS = 4000;
    std::atomic<size_t> laneDone{0};
    std::atomic<bool> allInside{false};
    auto task = sc::threading::spawn([&] {
      std::mutex mutex;
      std::set<std::thread::id> inside;
      const auto self = std::this_thread::get_id();
      // All of them, not the half a loop of the lane gets by default.
      sc::threading::parallelFor(LANE_CHUNKS, 1, workers, [&](size_t, size_t) {
        if (std::this_thread::get_id() != self) {
          const std::scoped_lock lock(mutex);
          inside.insert(std::this_thread::get_id());
          if (inside.size() == workers) {
            allInside = true;
          }
        }
        std::this_thread::sleep_for(1ms);
        laneDone.fetch_add(1);
      });
    });
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!allInside.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    REQUIRE(allInside.load());

    std::atomic<size_t> frameHelpers{0};
    const auto self = std::this_thread::get_id();
    sc::threading::parallelFor(64, 1, [&](size_t, size_t) {
      if (std::this_thread::get_id() != self) {
        frameHelpers.fetch_add(1);
      }
      std::this_thread::sleep_for(1ms);
    });
    REQUIRE(frameHelpers.load() > 0);
    REQUIRE(laneDone.load() < LANE_CHUNKS);
    task.get();
    REQUIRE(laneDone.load() == LANE_CHUNKS);
  }
}

TEST_CASE("Background shutdown", "[threading][background]") {
  sc::threading::init();

  // One task per thread that runs until it is asked to stop, and one more
  // that waits behind them.
  std::vector<sc::threading::BackgroundTask<bool>> running;
  std::atomic<size_t> started{0};
  for (uint32_t i = 0; i < sc::threading::defaultBackgroundCount(); ++i) {
    running.push_back(sc::threading::spawn([&](const std::stop_token &stop) {
      started.fetch_add(1);
      while (!stop.stop_requested()) {
        std::this_thread::yield();
      }
      return true;
    }));
  }
  while (started.load() != running.size()) {
    std::this_thread::yield();
  }
  auto waiting = sc::threading::spawn([] { return 1; });

  sc::threading::shutdownBackground();
  for (auto &task: running) {
    REQUIRE(task.get());
  }
  REQUIRE_THROWS_AS(waiting.get(), std::future_error);

  // Starts again, as the pool does.
  sc::threading::initBackground();
  REQUIRE(sc::threading::spawn([] { return 3; }).get() == 3);
}

TEST_CASE("Background service", "[threading][background]") {
  SECTION("the destructor stops and joins it") {
    std::atomic<bool> started{false};
    std::atomic<bool> done{false};
    {
      sc::threading::Service service("sc-test-service", [&](const std::stop_token &stop) {
        started = true;
        while (!stop.stop_requested()) {
          std::this_thread::sleep_for(1ms);
        }
        done = true;
      });
      REQUIRE(service.running());
      waitFor(started);
    }
    REQUIRE(done);
  }

  SECTION("an exception is logged, not passed on") {
    std::atomic<bool> threw{false};
    sc::threading::Service service("sc-test-throw", [&](const std::stop_token &) {
      threw = true;
      throw std::runtime_error("service failed");
    });
    waitFor(threw);
  }

  SECTION("runs as a background thread") {
    std::atomic<int> background{-1};
    {
      sc::threading::Service service(
          "sc-test-flag", [&](const std::stop_token &) { background = sc::threading::onBackgroundThread() ? 1 : 0; });
    }
    REQUIRE(background == 1);
  }
}
