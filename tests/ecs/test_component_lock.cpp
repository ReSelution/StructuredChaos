#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "sc/ecs/component_lock.hpp"

using sc::ecs::internal::ComponentLock;

TEST_CASE("ComponentLock Basic Use", "[ecs][lock]") {
  ComponentLock lock;

  SECTION("Several Readers At Once") {
    lock.lock_shared();
    lock.lock_shared();
    lock.unlock_shared();
    lock.unlock_shared();

    // Free again: a writer gets in right away.
    lock.lock();
    lock.unlock();
  }

  SECTION("Works With The Standard Guards") {
    {
      std::unique_lock guard(lock);
    }
    {
      std::lock_guard guard(lock);
    }
    lock.lock_shared();
    lock.unlock_shared();
  }
}

TEST_CASE("ComponentLock Writers Exclude Each Other", "[ecs][lock]") {
  constexpr int thread_count = 8;
  constexpr int per_thread = 50'000;

  ComponentLock lock;
  // Deliberately not atomic: only the lock keeps the increments apart.
  long counter = 0;

  std::vector<std::thread> threads;
  for (int t = 0; t < thread_count; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < per_thread; ++i) {
        lock.lock();
        ++counter;
        lock.unlock();
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  REQUIRE(counter == static_cast<long>(thread_count) * per_thread);
}

TEST_CASE("ComponentLock Readers Never See A Writer At Work", "[ecs][lock]") {
  constexpr int writer_count = 2;
  constexpr int reader_count = 6;
  constexpr int writes_per_thread = 20'000;

  ComponentLock lock;
  // A writer changes both values, so readers must always find them equal.
  long first = 0;
  long second = 0;

  std::atomic<bool> stop{false};
  std::atomic<long> torn_reads{0};
  std::atomic<long> reads{0};

  std::vector<std::thread> readers;
  for (int t = 0; t < reader_count; ++t) {
    readers.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        lock.lock_shared();
        if (first != second) {
          torn_reads.fetch_add(1, std::memory_order_relaxed);
        }
        lock.unlock_shared();
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::vector<std::thread> writers;
  for (int t = 0; t < writer_count; ++t) {
    writers.emplace_back([&]() {
      for (int i = 0; i < writes_per_thread; ++i) {
        lock.lock();
        ++first;
        ++second;
        lock.unlock();
      }
    });
  }

  for (auto &thread : writers) {
    thread.join();
  }
  stop.store(true);
  for (auto &thread : readers) {
    thread.join();
  }

  REQUIRE(torn_reads.load() == 0);
  REQUIRE(first == static_cast<long>(writer_count) * writes_per_thread);
  REQUIRE(second == first);
  REQUIRE(reads.load() > 0);
}
