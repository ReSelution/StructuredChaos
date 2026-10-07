#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
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

TEST_CASE("ComponentLock Writer Gets In Between Overlapping Readers",
          "[ecs][lock]") {
  // Every reader holds the lock much longer than it stays away from it, so
  // with eight of them the lock is practically never free on its own. A
  // writer only gets its turn because new readers stay out while it waits.
  constexpr int reader_count = 8;
  constexpr int writes = 50;
  constexpr auto hold_time = std::chrono::microseconds(200);

  ComponentLock lock;
  std::atomic<bool> stop{false};
  std::atomic<int> written{0};

  std::vector<std::thread> readers;
  for (int t = 0; t < reader_count; ++t) {
    readers.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        lock.lock_shared();
        std::this_thread::sleep_for(hold_time);
        lock.unlock_shared();
      }
    });
  }

  std::thread writer([&]() {
    for (int i = 0; i < writes; ++i) {
      lock.lock();
      written.fetch_add(1, std::memory_order_relaxed);
      lock.unlock();
    }
  });

  // Generous limit: with the waiting mark this takes a few hundredths of a
  // second. Once the readers stop, the writer finishes either way.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (written.load(std::memory_order_relaxed) < writes &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const int reached = written.load();

  stop.store(true);
  for (auto &thread : readers) {
    thread.join();
  }
  writer.join();

  REQUIRE(reached == writes);
}
