#pragma once

#include "atomic_queue.hpp"
#include "internal.hpp"
#include "types.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <semaphore>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
    for (const auto &q : pQueues) {
      if (!q.empty()) {
        return false;
      }
    }
    return true;
  }

  template <Priority p> void enqueue(MoveOnlyFunction &&task) {
    pQueues[static_cast<size_t>(p)].push(std::move(task));
  }

  template <Priority p, typename Arg> void emplace(Arg &&arg) {
    pQueues[static_cast<size_t>(p)].emplace(
        MoveOnlyFunction(std::forward<Arg>(arg)));
  }
};

inline std::mutex threadMutex;
inline std::vector<std::jthread> threads;
inline std::vector<std::jthread> longRunningThreads;
inline std::condition_variable_any wait_cv;

static constexpr size_t MAX_TASKS =
    QUEUE_CAP * static_cast<size_t>(Priority::PriorityCount);
inline std::counting_semaphore<MAX_TASKS> pool_sema{0};

inline Queues queues;
inline std::mutex queueMutex;
inline std::vector<std::unique_ptr<aQueue>> workerStores;
inline std::atomic<uint64_t> available{0};
inline thread_local int32_t threadId = 0;

inline std::atomic<size_t> pending_tasks{0};
inline std::atomic<size_t> active_tasks{0};

inline void on_task_enqueued(size_t count = 1) {
  pending_tasks.fetch_add(count, std::memory_order_relaxed);
  QueueSize::record(count); // Stat existiert oder ist No-Op
}

MoveOnlyFunction helpThread(int id);

void workerThread(const std::stop_token &st, int id);

void signalWork(size_t count);

void shutdown();

void init_impl(uint32_t numThreads = std::thread::hardware_concurrency() - 1);

void wait_until_finished();

} // namespace sc::threading::impl
