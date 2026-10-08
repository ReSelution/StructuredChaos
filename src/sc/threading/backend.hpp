#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <semaphore>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "atomic_queue.hpp"
#include "internal.hpp"
#include "types.hpp"

namespace sc::threading::impl {

  void set_thread_name([[maybe_unused]] std::string_view name);
  using aQueue = AtomicQueue<MoveOnlyFunction, QUEUE_CAP>;

  struct Queues {
    std::array<aQueue, static_cast<size_t>(Priority::PriorityCount)> pQueues;
    Queues() = default;

    bool try_pop_any(MoveOnlyFunction &task, int threadId) {
      for (auto &q : pQueues) {
        if (q.try_pop(task, threadId)) {
          return true;
        }
      }
      return false;
    }

    [[nodiscard]] bool was_empty() const {
      return std::ranges::all_of(pQueues, [](const auto &q) { return q.empty(); });
    }

    template <Priority p> void enqueue(MoveOnlyFunction &&task) {
      pQueues[static_cast<size_t>(p)].push(std::move(task));
    }

    template <Priority p, typename Arg> void emplace(Arg &&arg) {
      pQueues[static_cast<size_t>(p)].emplace(MoveOnlyFunction(std::forward<Arg>(arg)));
    }
  };

  inline std::mutex threadMutex;
  inline std::vector<std::jthread> threads;
  inline std::vector<std::jthread> longRunningThreads;
  inline std::condition_variable_any wait_cv;

  static constexpr size_t MAX_TASKS = QUEUE_CAP * static_cast<size_t>(Priority::PriorityCount);
  inline std::counting_semaphore<MAX_TASKS> pool_sema{0};

  inline Queues queues;
  inline std::mutex queueMutex;
  inline std::vector<std::unique_ptr<aQueue>> workerStores;
  inline std::atomic<uint64_t> available{0};
  inline thread_local int32_t threadId = 0;

  // Tasks that were enqueued and have not finished yet.
  inline std::atomic<size_t> outstanding_tasks{0};
  // Threads currently blocked in wait_until_finished.
  inline std::atomic<size_t> idle_waiters{0};

  // Has to be called before the tasks are pushed: a worker may pop and finish a
  // task right away, and it must not find the counter without that task.
  inline void on_task_enqueued(size_t count = 1) {
    outstanding_tasks.fetch_add(count, std::memory_order_relaxed);
    QueueSize::record(count); // stat exists or is a no-op
  }

  MoveOnlyFunction helpThread(int id);

  void workerThread(const std::stop_token &st, int id);

  void signalWork(size_t count);

  void shutdown();

  void init_impl(uint32_t numThreads = std::thread::hardware_concurrency() - 1);

  void wait_until_finished();

} // namespace sc::threading::impl
