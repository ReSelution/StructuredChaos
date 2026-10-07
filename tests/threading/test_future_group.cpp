#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "sc/threading/future_group.hpp"
#include "sc/threading/threading.hpp"

namespace {

using sc::threading::FutureGroup;

static_assert(std::is_default_constructible_v<FutureGroup<int>>);
static_assert(std::is_nothrow_move_constructible_v<FutureGroup<int>>);
static_assert(std::is_nothrow_move_assignable_v<FutureGroup<int>>);
static_assert(!std::is_copy_constructible_v<FutureGroup<int>>);
static_assert(!std::is_copy_assignable_v<FutureGroup<int>>);
static_assert(std::is_same_v<decltype(std::declval<FutureGroup<int> &>().get()),
                             std::vector<int>>);
static_assert(
    std::is_void_v<decltype(std::declval<FutureGroup<void> &>().get())>);

struct SFOBreaker {
  std::array<std::byte, 100> weight{};
};

// A group over fresh promises, which the test resolves by hand.
template <typename T> struct Pending {
  std::vector<std::promise<T>> promises;
  FutureGroup<T> group;

  explicit Pending(size_t count) : promises(count) {
    group.reserve(count);
    for (auto &p : promises) {
      group.push(p.get_future());
    }
  }
};

} // namespace

TEST_CASE("FutureGroup Empty Group", "[threading][future_group]") {
  FutureGroup<int> group;

  REQUIRE(group.empty());
  REQUIRE(group.size() == 0);
  REQUIRE(group.valid_count() == 0);
  REQUIRE(group.completed_count() == 0);
  REQUIRE(group.ready());
  REQUIRE(group.progress() == 1.0f);

  group.wait();
  REQUIRE(group.get().empty());
}

TEST_CASE("FutureGroup Tracks Completion", "[threading][future_group]") {
  SECTION("Progress Follows The Resolved Futures") {
    Pending<int> pending(4);
    auto &group = pending.group;

    REQUIRE(group.size() == 4);
    REQUIRE_FALSE(group.empty());
    REQUIRE(group.valid_count() == 4);
    REQUIRE(group.completed_count() == 0);
    REQUIRE_FALSE(group.ready());
    REQUIRE(group.progress() == 0.0f);

    pending.promises[2].set_value(2);
    REQUIRE(group.completed_count() == 1);
    REQUIRE(group.progress() == 0.25f);
    REQUIRE_FALSE(group.ready());

    pending.promises[0].set_value(0);
    REQUIRE(group.completed_count() == 2);
    REQUIRE(group.progress() == 0.5f);

    pending.promises[1].set_value(1);
    pending.promises[3].set_value(3);
    REQUIRE(group.completed_count() == 4);
    REQUIRE(group.progress() == 1.0f);
    REQUIRE(group.ready());
  }

  SECTION("An Exception Counts As Completed") {
    Pending<int> pending(2);

    pending.promises[0].set_exception(
        std::make_exception_ptr(std::runtime_error("failed")));

    REQUIRE(pending.group.completed_count() == 1);
    REQUIRE_FALSE(pending.group.ready());
  }

  SECTION("A Broken Promise Counts As Completed") {
    FutureGroup<int> group;
    {
      std::promise<int> abandoned;
      group.push(abandoned.get_future());
    }

    REQUIRE(group.ready());
    REQUIRE_THROWS_AS(group.get(), std::future_error);
  }

  SECTION("Invalid Futures Are Skipped") {
    FutureGroup<int> group;
    group.push(std::future<int>{});
    std::promise<int> p;
    group.push(p.get_future());

    REQUIRE(group.size() == 2);
    REQUIRE(group.valid_count() == 1);
    REQUIRE(group.completed_count() == 1);
    REQUIRE_FALSE(group.ready());

    p.set_value(7);
    group.wait();
    REQUIRE(group.ready());
    REQUIRE(group.get() == std::vector<int>{7});
  }
}

TEST_CASE("FutureGroup Collects Results", "[threading][future_group]") {
  SECTION("Results Keep Push Order, Not Completion Order") {
    Pending<int> pending(5);

    for (int i = 4; i >= 0; --i) {
      pending.promises[static_cast<size_t>(i)].set_value(i * 10);
    }

    REQUIRE(pending.group.get() == std::vector<int>{0, 10, 20, 30, 40});
  }

  SECTION("get Consumes The Futures") {
    Pending<int> pending(3);
    for (size_t i = 0; i < 3; ++i) {
      pending.promises[i].set_value(static_cast<int>(i));
    }
    auto &group = pending.group;

    REQUIRE(group.get().size() == 3);

    REQUIRE(group.size() == 3);
    REQUIRE(group.valid_count() == 0);
    REQUIRE(group.ready());
    REQUIRE(group.get().empty());
  }

  SECTION("Void Group") {
    Pending<void> pending(3);
    for (auto &p : pending.promises) {
      p.set_value();
    }

    REQUIRE(pending.group.ready());
    pending.group.get();
    REQUIRE(pending.group.valid_count() == 0);
  }

  SECTION("Move-Only Results") {
    Pending<std::unique_ptr<int>> pending(2);
    pending.promises[0].set_value(std::make_unique<int>(1));
    pending.promises[1].set_value(std::make_unique<int>(2));

    auto results = pending.group.get();

    REQUIRE(results.size() == 2);
    REQUIRE(*results[0] == 1);
    REQUIRE(*results[1] == 2);
  }

  SECTION("get Rethrows A Stored Exception") {
    Pending<int> pending(3);
    pending.promises[0].set_value(1);
    pending.promises[1].set_exception(
        std::make_exception_ptr(std::runtime_error("failed")));
    pending.promises[2].set_value(3);

    REQUIRE_THROWS_AS(pending.group.get(), std::runtime_error);
    // The futures behind the failing one are consumed as well.
    REQUIRE(pending.group.valid_count() == 0);
    REQUIRE(pending.group.get().empty());
  }

  SECTION("The First Exception In Push Order Wins") {
    Pending<int> pending(3);
    // Resolved back to front, so completion order differs from push order.
    pending.promises[2].set_exception(
        std::make_exception_ptr(std::logic_error("second")));
    pending.promises[1].set_exception(
        std::make_exception_ptr(std::runtime_error("first")));
    pending.promises[0].set_value(1);

    REQUIRE_THROWS_AS(pending.group.get(), std::runtime_error);
    REQUIRE(pending.group.valid_count() == 0);
  }

  SECTION("get Waits For The Remaining Futures Before Rethrowing") {
    Pending<int> pending(3);
    std::atomic<bool> all_resolved{false};
    pending.promises[0].set_exception(
        std::make_exception_ptr(std::runtime_error("failed")));

    std::thread producer([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      pending.promises[1].set_value(1);
      all_resolved.store(true, std::memory_order_release);
      pending.promises[2].set_value(2);
    });

    REQUIRE_THROWS_AS(pending.group.get(), std::runtime_error);
    REQUIRE(all_resolved.load(std::memory_order_acquire));
    REQUIRE(pending.group.valid_count() == 0);
    producer.join();
  }

  SECTION("Void get Rethrows A Stored Exception") {
    Pending<void> pending(2);
    pending.promises[0].set_value();
    pending.promises[1].set_exception(
        std::make_exception_ptr(std::runtime_error("failed")));

    REQUIRE_THROWS_AS(pending.group.get(), std::runtime_error);
    REQUIRE(pending.group.valid_count() == 0);
  }

  SECTION("Void get Consumes The Futures Behind A Failure") {
    Pending<void> pending(3);
    pending.promises[0].set_exception(
        std::make_exception_ptr(std::runtime_error("failed")));
    pending.promises[1].set_value();
    pending.promises[2].set_value();

    REQUIRE_THROWS_AS(pending.group.get(), std::runtime_error);
    REQUIRE(pending.group.valid_count() == 0);
  }
}

TEST_CASE("FutureGroup Blocks Until Done", "[threading][future_group]") {
  SECTION("wait Returns Only After Every Future Is Resolved") {
    Pending<int> pending(8);
    std::atomic<size_t> resolved{0};

    std::thread producer([&] {
      for (size_t i = 0; i < pending.promises.size(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        resolved.fetch_add(1, std::memory_order_release);
        pending.promises[i].set_value(static_cast<int>(i));
      }
    });

    pending.group.wait();
    REQUIRE(resolved.load(std::memory_order_acquire) == 8);
    REQUIRE(pending.group.ready());
    // wait does not consume.
    REQUIRE(pending.group.valid_count() == 8);

    producer.join();
  }

  SECTION("get Blocks For Pending Results") {
    Pending<int> pending(4);

    std::thread producer([&] {
      for (size_t i = 0; i < pending.promises.size(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        pending.promises[i].set_value(static_cast<int>(i) + 1);
      }
    });

    REQUIRE(pending.group.get() == std::vector<int>{1, 2, 3, 4});
    producer.join();
  }
}

TEST_CASE("FutureGroup Ownership", "[threading][future_group]") {
  SECTION("Constructs From A Vector Of Futures") {
    std::vector<std::promise<std::string>> promises(2);
    std::vector<std::future<std::string>> futures;
    for (auto &p : promises) {
      futures.push_back(p.get_future());
    }

    FutureGroup<std::string> group{std::move(futures)};
    promises[0].set_value("a");
    promises[1].set_value("b");

    REQUIRE(group.size() == 2);
    REQUIRE(group.get() == std::vector<std::string>{"a", "b"});
  }

  SECTION("Move Construction Transfers The Futures") {
    Pending<int> pending(2);
    pending.promises[0].set_value(1);
    pending.promises[1].set_value(2);

    FutureGroup<int> moved{std::move(pending.group)};

    REQUIRE(moved.size() == 2);
    REQUIRE(moved.get() == std::vector<int>{1, 2});
  }

  SECTION("Move Assignment Replaces The Futures") {
    Pending<int> source(1);
    Pending<int> target(3);
    source.promises[0].set_value(9);

    target.group = std::move(source.group);

    REQUIRE(target.group.size() == 1);
    REQUIRE(target.group.get() == std::vector<int>{9});
  }

  SECTION("clear Drops Everything") {
    Pending<int> pending(3);

    pending.group.clear();

    REQUIRE(pending.group.empty());
    REQUIRE(pending.group.size() == 0);
    REQUIRE(pending.group.ready());
    REQUIRE(pending.group.progress() == 1.0f);
  }
}

TEST_CASE("FutureGroup From enqueueBatch", "[threading][future_group]") {
  sc::threading::init();

  constexpr size_t task_count = 1000;
  std::vector<int> tasks(task_count);
  std::iota(tasks.begin(), tasks.end(), 0);

  SECTION("Small Tasks Return One Result Per Item In Order") {
    FutureGroup<int> group = sc::threading::enqueueBatch(
        std::move(tasks), [](int, int value) { return value * 2; });

    REQUIRE(group.size() == task_count);

    auto results = group.get();
    REQUIRE(results.size() == task_count);
    for (size_t i = 0; i < task_count; ++i) {
      REQUIRE(results[i] == static_cast<int>(i) * 2);
    }
  }

  SECTION("Tasks That Do Not Fit The Small Function Buffer") {
    SFOBreaker breaker;
    breaker.weight.fill(std::byte{1});

    FutureGroup<size_t> group = sc::threading::enqueueBatch(
        std::move(tasks), [breaker](int, int value) {
          return static_cast<size_t>(value) +
                 std::to_integer<size_t>(breaker.weight[0]);
        });

    auto results = group.get();
    REQUIRE(results.size() == task_count);
    for (size_t i = 0; i < task_count; ++i) {
      REQUIRE(results[i] == i + 1);
    }
  }

  SECTION("Extra Arguments Are Forwarded") {
    FutureGroup<int> group = sc::threading::enqueueBatch(
        std::move(tasks),
        [](int, int value, int offset) { return value + offset; }, 100);

    auto results = group.get();
    REQUIRE(results.size() == task_count);
    REQUIRE(results.front() == 100);
    REQUIRE(results.back() == static_cast<int>(task_count) + 99);
  }

  SECTION("Accepts A Priority") {
    auto group = sc::threading::enqueueBatch<sc::threading::Priority::High>(
        std::move(tasks), [](int, int value) { return std::to_string(value); });

    auto results = group.get();
    REQUIRE(results.size() == task_count);
    REQUIRE(results[42] == "42");
  }

  SECTION("Becomes Ready Once The Pool Is Idle") {
    auto group = sc::threading::enqueueBatch(
        std::move(tasks), [](int, int value) { return value; });

    sc::threading::wait_until_finished();

    REQUIRE(group.ready());
    REQUIRE(group.completed_count() == task_count);
    REQUIRE(group.progress() == 1.0f);
  }

  SECTION("A Throwing Task Surfaces Through get") {
    auto group = sc::threading::enqueueBatch(
        std::move(tasks), [](int, int value) -> int {
          if (value == 500) {
            throw std::runtime_error("task 500");
          }
          return value;
        });

    group.wait();
    REQUIRE(group.ready());
    REQUIRE_THROWS_AS(group.get(), std::runtime_error);
    REQUIRE(group.valid_count() == 0);
    sc::threading::wait_until_finished();
  }

  SECTION("A Throwing Task On The Merged Path Surfaces Through get") {
    SFOBreaker breaker;

    auto group = sc::threading::enqueueBatch(
        std::move(tasks), [breaker](int, int value) -> int {
          (void)breaker;
          if (value == 500) {
            throw std::runtime_error("task 500");
          }
          return value;
        });

    REQUIRE_THROWS_AS(group.get(), std::runtime_error);
    REQUIRE(group.valid_count() == 0);
    sc::threading::wait_until_finished();
  }
}
