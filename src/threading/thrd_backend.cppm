module;
#include <condition_variable>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>


#ifdef _WIN32
#include <processthreadsapi.h>
#include <windows.h>
#elif __linux__
#include <cstring>
#include <pthread.h>
#endif
export module sc.threading:backend;
import :types;
export import :atomic_queue;
import :internal;


namespace sc::threading::impl {
  static void set_thread_name([[maybe_unused]] std::string_view name) {
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
  export using aQueue = AtomicQueue<MoveOnlyFunction, QUEUE_CAP>;

  export struct Queues {
    std::array<aQueue, static_cast<size_t>(Priority::PriorityCount)> pQueues;
    Queues() {};

    bool try_pop_any(MoveOnlyFunction &task, int threadId) {
      return (pQueues[0].try_pop(task, threadId) || pQueues[1].try_pop(task, threadId) ||
              pQueues[2].try_pop(task, threadId));
    }

    [[nodiscard]] bool was_empty() const { return (pQueues[0].empty() && pQueues[1].empty() && pQueues[2].empty()); }

    template<Priority p>
    void enqueue(MoveOnlyFunction &&task) {
      pQueues[static_cast<size_t>(p)].push(std::move(task));
    }
    template<Priority p, typename Args>
    void emplace(Args &&args) {
      pQueues[static_cast<size_t>(p)].emplace(std::forward<Args>(args));
    }
  };


  static inline std::mutex threadMutex;
  static inline std::vector<std::jthread> threads;
  static inline std::vector<std::jthread> longRunningThreads;
  static inline std::condition_variable_any wait_cv;

  static constexpr size_t MAX_TASKS = QUEUE_CAP * static_cast<size_t>(Priority::PriorityCount);
  inline std::counting_semaphore<MAX_TASKS> pool_sema{0};

  export inline Queues queues;
  static inline std::mutex queueMutex;
  export inline std::vector<std::unique_ptr<aQueue>> workerStores;
  static inline std::atomic<uint64_t> available{0};
  static inline thread_local int32_t threadId = 0;

  static MoveOnlyFunction helpThread(int id) {
    uint64_t mask = available.load(std::memory_order_acquire);
    if (mask == 0)
      return {};

    const auto n     = workerStores.size();
    uint32_t shift   = (id + 1) % n;
    uint64_t rotated = (mask >> shift) | (mask << (n - shift));
    rotated &= (1ULL << n) - 1;

    int rotatedIdx     = std::countr_zero(rotated);
    uint32_t targetIdx = (rotatedIdx + shift) % n;

    MoveOnlyFunction task;
    if (workerStores[targetIdx]->try_pop(task, id))
      return task;
    return {};
  }

  static void workerThread(const std::stop_token &st, int id) {
    threadId = id;
    while (!st.stop_requested()) {
      MoveOnlyFunction task;
      if (queues.try_pop_any(task, threadId)) {
        ActiveTask::record(1);
        QueueSize::record(-1);
        pool_sema.try_acquire();

        task(id);

        ActiveTask::record(-1);
        PoolThroughput::record(1);
        continue;
      }
      task = helpThread(id);
      if (task) {
        pool_sema.try_acquire();
        task(id);
        continue;
      }

      if (queues.was_empty() && ActiveTask::m_storage.value.load(std::memory_order_relaxed) == 0) [[unlikely]] {
        wait_cv.notify_all();
      }
      pool_sema.try_acquire_for(std::chrono::milliseconds(10));
    }
  }

  export void signalWork(size_t count) { pool_sema.release(count); }

  static void shutdown() {
    {
      std::lock_guard lock(threadMutex);
      for (auto &t: threads)
        t.request_stop();
      for (auto &t: longRunningThreads)
        t.request_stop();
    }
    signalWork(threads.size());
    threads.clear();
    longRunningThreads.clear();
  }

  export void init_impl(uint32_t numThreads = std::thread::hardware_concurrency() - 1) {
    if (!threads.empty()) [[unlikely]]
      return;

    const size_t longCount   = longRunningThreads.size();
    const size_t maxHardware = std::thread::hardware_concurrency();
    const size_t threadCount = std::min<size_t>(numThreads, maxHardware - 1 - longCount);
    const size_t totalSlots  = 1 + longCount + threadCount;

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

  export template<Priority P>
  void pushBatch(std::vector<MoveOnlyFunction> &&batch) {
    for (auto &it: batch)
      queues.enqueue<P>(std::move(it));
    QueueSize::record(batch.size());
    signalWork(batch.size());
  }

  export void wait_until_finished() {
    std::unique_lock lock(queueMutex);
    wait_cv.wait(lock, [] {
      return QueueSize::m_storage.value.load(std::memory_order_relaxed) == 0 &&
             ActiveTask::m_storage.value.load(std::memory_order_relaxed) == 0;
    });
  }

} // namespace sc::threading::impl
