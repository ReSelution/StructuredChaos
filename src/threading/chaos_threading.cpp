#include "chaos_threading.hpp"
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic_queue/atomic_queue.h>
#include <semaphore>
#include "tracy/Tracy.hpp"

#ifdef _WIN32
#include <windows.h>
#include <processthreadsapi.h>
#include <codecvt>
#include <locale>
#elif __linux__

#include <pthread.h>

#endif


namespace SC {


  using aQueue = atomic_queue::AtomicQueueB2<MoveOnlyFunction, std::allocator<MoveOnlyFunction>>;

  struct alignas(64) WorkerData {
    explicit WorkerData(uint32_t cap) : queue(cap) {}

    aQueue queue;
  };

  struct alignas(64) Queues {
    using Priority = ChaosThreading::Priority;
    std::array<aQueue, static_cast<size_t>(Priority::PriorityCount)> pQueues;

    Queues() : pQueues{aQueue(QUEUE_CAP), aQueue(QUEUE_CAP), aQueue(QUEUE_CAP)} {

    }

    bool try_pop_any(MoveOnlyFunction &task) {
      return try_pop_impl(task, std::make_index_sequence<static_cast<size_t>(Priority::PriorityCount)>{});
    }

    [[nodiscard]] bool was_empty() const {
      return was_empty_impl(std::make_index_sequence<static_cast<size_t>(Priority::PriorityCount)>{});
    }

    template<Priority p>
    void enqueue(MoveOnlyFunction &&task) {
      constexpr int index = static_cast<int>(p);
      pQueues[index].push(std::move(task));
    }

  private:

    template<size_t... Is>
    bool try_pop_impl(MoveOnlyFunction &task, std::index_sequence<Is...>) {
      return (pQueues[Is].try_pop(task) || ...); // Fold Expression (C++17)
    }

    template<size_t... Is>
    [[nodiscard]] bool was_empty_impl(std::index_sequence<Is...>) const {
      return (pQueues[Is].was_empty() && ...);
    }
  };

  static std::mutex threadMutex;
  static std::vector<std::jthread> threads;
  static std::vector<std::jthread> longRunningThreads;

  static std::condition_variable_any cv;
  static std::condition_variable_any wait_cv;

  constexpr size_t MAX_TASKS = QUEUE_CAP * static_cast<int32_t>(ChaosThreading::Priority::PriorityCount);
  alignas(64) static std::counting_semaphore<MAX_TASKS> pool_sema{0};

  static Queues queues;
  static std::mutex queueMutex;

  static std::vector<std::unique_ptr<WorkerData>> workerStores;
  static std::atomic<uint64_t> available{0};

  static thread_local int32_t threadId = 0;

  void set_thread_name(std::string_view name) {
#ifdef _WIN32
    wchar_t wName[64];

    swprintf(wName, 64, L"%hs", name);
    SetThreadDescription(GetCurrentThread(), wName);
#elif __linux__
    char shortName[16];
    strncpy(shortName, name.data(), 15);
    shortName[15] = '\0';
    pthread_setname_np(pthread_self(), shortName);
#endif
  }

  size_t ChaosThreading::getNumThreads() {
    return threads.size() + 1; // +1 to include main Thread
  }

  int32_t ChaosThreading::getThreadId() {
    return threadId;
  }

  MoveOnlyFunction ChaosThreading::helpThread(int id) {
    uint64_t mask = available.load(std::memory_order_acquire);
    if (mask == 0) return {};

    const auto n = workerStores.size();

    uint32_t shift = (id + 1) % n;
    uint64_t rotated = (mask >> shift) | (mask << (n - shift));
    rotated &= (1ULL << n) - 1;

    int rotatedIdx = std::countr_zero(rotated);
    uint32_t targetIdx = (rotatedIdx + shift) % n;

    MoveOnlyFunction task;
    if (workerStores[targetIdx]->queue.try_pop(task)) {
      return task;
    }
    return {};
  }

  void workerThread(const std::stop_token &st, int id) {
    char name[16];
    snprintf(name, sizeof(name), "ChaosPool-%d", id);
    //set_thread_name(name);
    threadId = id;
    //tracy::SetThreadName(name);
    while (!st.stop_requested()) {
      MoveOnlyFunction task;
      if (queues.try_pop_any(task)) {
        ActiveTask::record(1);
        QueueSize::record(-1);
        task(id);
        pool_sema.acquire();
        ActiveTask::record(-1);
        CHAOS_RECORD(PoolThroughput, 1)
        continue;
      }
      task = ChaosThreading::helpThread(id);
      if (task) {
        task(id);
        continue;
      }

//      if (pool_sema.try_acquire()) {
//        continue;
//      }
//      bool foundWork = false;
//      for (int i = 0; i < 5000; ++i) {
//        if (!queues.was_empty() || available.load(std::memory_order_acquire) != 0) {
//          foundWork = true;
//          break;
//        }
//        _mm_pause();
//      }
//      if (foundWork) continue;
      if (queues.was_empty() && ActiveTask::m_storage.value.load() == 0) {
        wait_cv.notify_all();
      }
      pool_sema.try_acquire_for(std::chrono::milliseconds(10));
    }
  }

  void signalWork(size_t count) {
    pool_sema.release(count);
  }

  void shutdown() {

    {
      std::lock_guard lock(threadMutex);
      for (auto &t: threads) {
        t.request_stop();
      }
      for (auto &t: longRunningThreads) {
        t.request_stop();
      }
    }
    signalWork(threads.size());
    threads.clear();
    longRunningThreads.clear();
  }

  void ChaosThreading::init(uint32_t numThreads) {
    if (!threads.empty()) return;
    auto maxThreads = std::thread::hardware_concurrency() - 1 - longRunningThreads.size();
    auto threadCount = std::min(std::max<size_t>(numThreads, 1), maxThreads);

    workerStores.reserve(threadCount);
    for (uint32_t i = 0; i < threadCount; ++i) {
      workerStores.push_back(std::make_unique<WorkerData>(threadCount * HELPER_TASK_MULTIPLYER));
    }

    CHAOS_START(PoolThroughput)
    std::atexit(shutdown);
    for (uint32_t i = 0; i < threadCount; ++i) {
      threads.emplace_back(workerThread, i + longRunningThreads.size());
    }
  }

  template<ChaosThreading::Priority P>
  void ChaosThreading::pushTask(MoveOnlyFunction &&task) {
    queues.enqueue<P>(std::move(task));
    QueueSize::record(1);
    signalWork(1);
  }

  template void ChaosThreading::pushTask<ChaosThreading::Priority::High>(MoveOnlyFunction &&task);

  template void ChaosThreading::pushTask<ChaosThreading::Priority::Normal>(MoveOnlyFunction &&task);

  template void ChaosThreading::pushTask<ChaosThreading::Priority::Low>(MoveOnlyFunction &&task);


  template<ChaosThreading::Priority P>
  void ChaosThreading::pushBatch(std::vector<MoveOnlyFunction> &&batch) {
    for (auto &it: batch) {
      queues.enqueue<P>(std::move(it));
    }
    QueueSize::record(batch.size());
    signalWork(batch.size());
  }

  template void ChaosThreading::pushBatch<ChaosThreading::Priority::High>(std::vector<MoveOnlyFunction> &&);

  template void ChaosThreading::pushBatch<ChaosThreading::Priority::Normal>(std::vector<MoveOnlyFunction> &&);

  template void ChaosThreading::pushBatch<ChaosThreading::Priority::Low>(std::vector<MoveOnlyFunction> &&);

  void ChaosThreading::pushLongTaskInternal(std::string_view name, std::function<void(std::stop_token)> task) {
    if (!threads.empty()) {
      throw std::logic_error("Creating Long running Thread after init Call");
    }

    CHAOS_RECORD(LongRunningThreads, 1)
    std::lock_guard lock(threadMutex);
    std::erase_if(longRunningThreads, [](const std::jthread &t) { return !t.joinable(); });

    longRunningThreads.emplace_back([name, task = std::move(task)](const std::stop_token &st) {
      std::string nameStr(name);
      set_thread_name(nameStr);
      threadId = LongRunningThreads::m_storage.value;
      task(st);
      CHAOS_RECORD(LongRunningThreads, -1);
    });

  }

  void ChaosThreading::pushHelperTask(MoveOnlyFunction &&task) {
    const auto id = getThreadId();
    auto &store = workerStores[id];
    store->queue.push(std::move(task));
    available.fetch_or(1u << id, std::memory_order_release);
    signalWork(1);
  }


  void ChaosThreading::wait_until_finished() {
    std::unique_lock lock(queueMutex);
    wait_cv.wait(lock, [] {
      return QueueSize::m_storage.value.load() == 0 && ActiveTask::m_storage.value.load() == 0;
    });
  }

  void ChaosThreading::doWork() {
    const auto id = getThreadId();
    auto &store = workerStores[id];
    MoveOnlyFunction task;
    while (store->queue.try_pop(task)) {
      task(id);
    }
    available.fetch_and(~(1u << id), std::memory_order_release);
  }
}
