#include "backend.hpp"

#include <cstddef>

#include "threading/types.hpp"

namespace sc::threading::impl {

  static inline void on_task_started() {
    pool_sema.try_acquire();
    ActiveTask::record(1);
    QueueSize::record(-1);
  }

  // The counter and the waiter count are read and written with the default
  // sequentially consistent ordering on purpose: either the waiter sees the
  // counter at zero, or the finishing thread sees the waiter and wakes it.
  static inline void on_task_finished() {
    ActiveTask::record(-1);
    if (outstanding_tasks.fetch_sub(1) == 1 && idle_waiters.load() != 0) {
      // Taking the mutex keeps the notification from slipping in between the
      // waiter's check of the counter and its going to sleep.
      {
        std::scoped_lock lock(queueMutex);
      }
      wait_cv.notify_all();
    }
  }

  void wait_until_finished() {
    idle_waiters.fetch_add(1);
    {
      std::unique_lock lock(queueMutex);
      wait_cv.wait(lock, [] { return outstanding_tasks.load() == 0; });
    }
    idle_waiters.fetch_sub(1);
  }

  void init_impl(uint32_t numThreads) {
    if (!threads.empty()) {
      [[unlikely]] return;
    }

    const size_t longCount = longRunningThreads.size();
    const size_t maxHardware = std::thread::hardware_concurrency();
    const size_t threadCount = std::min<size_t>(numThreads, maxHardware - 1 - longCount);
    const size_t totalSlots = 1 + longCount + threadCount;

    workerStores.reserve(totalSlots);
    for (uint32_t i = 0; i < totalSlots; ++i) {
      workerStores.push_back(std::make_unique<aQueue>());
    }

    std::atexit(shutdown);
    for (uint32_t i = 0; i < threadCount; ++i) {
      threads.emplace_back(workerThread, i + 1 + longRunningThreads.size());
    }
    threadId = 0;
  }
  void signalWork(size_t count) { pool_sema.release(static_cast<std::ptrdiff_t>(count)); }

  void shutdown() {
    {
      std::scoped_lock lock(threadMutex);
      for (auto &t : threads) {
        t.request_stop();
      }
      for (auto &t : longRunningThreads) {
        t.request_stop();
      }
    }
    signalWork(threads.size());
    threads.clear();
    longRunningThreads.clear();
  }

  void workerThread(const std::stop_token &st, int id) {
    threadId = id;
    while (!st.stop_requested()) {
      MoveOnlyFunction task;
      if (queues.try_pop_any(task, threadId)) {
        on_task_started();
        task();
        on_task_finished();
        continue;
      }
      task = helpThread(id);
      if (task) {
        on_task_started();
        task();
        on_task_finished();
        continue;
      }

      pool_sema.try_acquire_for(std::chrono::milliseconds(10));
    }
  }

  void set_thread_name([[maybe_unused]] std::string_view name) {
#ifdef _WIN32
    // A string_view is not necessarily terminated, so its length is passed on.
    wchar_t wName[64];
    swprintf(wName, 64, L"%.*hs", static_cast<int>(name.size()), name.data());
    SetThreadDescription(GetCurrentThread(), wName);
#elif __linux__
    // Linux allows 15 characters plus the terminator.
    char shortName[16]{};
    name.copy(shortName, sizeof(shortName) - 1);
    pthread_setname_np(pthread_self(), shortName);
#endif
  }
  MoveOnlyFunction helpThread(int id) {
    uint64_t mask = available.load(std::memory_order_acquire);
    if (mask == 0) {
      return {};
    }

    const auto n = workerStores.size();
    uint32_t shift = (id + 1) % n;
    uint64_t rotated = (mask >> shift) | (mask << (n - shift));
    rotated &= (1ULL << n) - 1;

    int rotatedIdx = std::countr_zero(rotated);
    uint32_t targetIdx = (rotatedIdx + shift) % n;

    MoveOnlyFunction task;
    if (workerStores[targetIdx]->try_pop(task, id)) {
      return task;
    }
    return {};
  }
} // namespace sc::threading::impl
