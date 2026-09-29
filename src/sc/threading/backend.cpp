#include "threading/backend.hpp"
#include "threading/types.hpp"
#include <cstddef>

namespace sc::threading::impl {

inline void on_task_started() {
  pending_tasks.fetch_sub(1, std::memory_order_relaxed);
  active_tasks.fetch_add(1, std::memory_order_relaxed);
  pool_sema.try_acquire();
  ActiveTask::record(1);
  QueueSize::record(-1);
}

inline void on_task_finished() {
  active_tasks.fetch_sub(1, std::memory_order_release);
  ActiveTask::record(-1);
}

void wait_until_finished() {
  std::unique_lock lock(queueMutex);
  wait_cv.wait(lock, [] {
    return pending_tasks.load(std::memory_order_relaxed) == 0 &&
           active_tasks.load(std::memory_order_relaxed) == 0;
  });
}

void init_impl(uint32_t numThreads) {
  if (!threads.empty()) [[unlikely]]
    return;

  const size_t longCount = longRunningThreads.size();
  const size_t maxHardware = std::thread::hardware_concurrency();
  const size_t threadCount =
      std::min<size_t>(numThreads, maxHardware - 1 - longCount);
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
void signalWork(size_t count) { pool_sema.release(count); }

void shutdown() {
  {
    std::lock_guard lock(threadMutex);
    for (auto &t : threads)
      t.request_stop();
    for (auto &t : longRunningThreads)
      t.request_stop();
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
      task(id);
      on_task_finished();
      continue;
    }
    task = helpThread(id);
    if (task) {
      on_task_started();
      task(id);
      on_task_finished();
      continue;
    }

    if (queues.was_empty() &&
        ActiveTask::m_storage.value.load(std::memory_order_relaxed) == 0)
        [[unlikely]] {
      wait_cv.notify_all();
    }
    pool_sema.try_acquire_for(std::chrono::milliseconds(10));
  }
}

void set_thread_name([[maybe_unused]] std::string_view name) {
#ifdef _WIN32
  wchar_t wName[64];
  swprintf(wName, 64, L"%hs", name.data());
  SetThreadDescription(GetCurrentThread(), wName);
#elif __linux__
  char shortName[16];
  std::strncpy(shortName, name.data(), 15);
  shortName[15] = '\0';
  pthread_setname_np(pthread_self(), shortName);
#endif
}
MoveOnlyFunction helpThread(int id) {
  uint64_t mask = available.load(std::memory_order_acquire);
  if (mask == 0)
    return {};

  const auto n = workerStores.size();
  uint32_t shift = (id + 1) % n;
  uint64_t rotated = (mask >> shift) | (mask << (n - shift));
  rotated &= (1ULL << n) - 1;

  int rotatedIdx = std::countr_zero(rotated);
  uint32_t targetIdx = (rotatedIdx + shift) % n;

  MoveOnlyFunction task;
  if (workerStores[targetIdx]->try_pop(task, id))
    return task;
  return {};
}
} // namespace sc::threading::impl
