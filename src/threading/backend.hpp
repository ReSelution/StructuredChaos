#pragma once

#include "atomic_queue.hpp"
#include "threading/internal.hpp"
#include "threading/types.hpp"

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

  template <Priority p, typename Args> void emplace(Args &&args) {
    pQueues[static_cast<size_t>(p)].emplace(std::forward<Args>(args));
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

MoveOnlyFunction helpThread(int id);

void workerThread(const std::stop_token &st, int id);

void signalWork(size_t count);

void shutdown();

void init_impl(uint32_t numThreads = std::thread::hardware_concurrency() - 1);

template <Priority P> void pushBatch(std::vector<MoveOnlyFunction> &&batch) {
  for (auto &it : batch) {
    queues.enqueue<P>(std::move(it));
  }
  QueueSize::record(batch.size());
  signalWork(batch.size());
}

void wait_until_finished();

} // namespace sc::threading::impl
