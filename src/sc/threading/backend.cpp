#include "backend.hpp"

#include <cstddef>

#include "threading/types.hpp"

namespace sc::threading::impl {

  // Whether this thread was woken up for work it has not found yet.
  static thread_local bool wokenForWork = false;

  // Every piece of work comes with one wake-up. A thread that was woken up
  // for it has used that up already; one that found the work while it was
  // awake anyway takes the wake-up back, so that nobody gets up for nothing.
  static inline void take_wake_up() {
    if (wokenForWork) {
      wokenForWork = false;
    } else {
      // Nothing to take back if the woken thread was faster.
      (void) pool_sema.try_acquire();
    }
  }

  static inline void on_task_started() {
    take_wake_up();
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

  void workerThread(const std::stop_token &st, int id) {
    threadId = id;
    while (!st.stop_requested()) {
      // First: a thread is waiting for these to be done.
      if (helpParallel()) {
        continue;
      }
      MoveOnlyFunction task;
      if (queues.try_pop_any(task, threadId)) {
        on_task_started();
        task();
        on_task_finished();
        continue;
      }

      wokenForWork = pool_sema.try_acquire_for(std::chrono::milliseconds(10));
    }
  }

  namespace {
    // Never 0: that is what the queue keeps in an empty place.
    constexpr uint64_t ticketOf(uint64_t generation, size_t slot) {
      return (generation << ParallelSlot::HELPER_BITS) | (slot + 1);
    }
  } // namespace

  ParallelSlot *shareParallel(ParallelJob &job, size_t helpers) {
    helpers = std::min(helpers, threads.size());
    if (helpers == 0) {
      return nullptr;
    }

    // Every thread starts at a place of its own, so that loops of different
    // threads do not meet in the same slots.
    ParallelSlot *slot = nullptr;
    size_t index = static_cast<size_t>(threadId) % PARALLEL_SLOTS;
    for (size_t tried = 0; tried < PARALLEL_SLOTS; ++tried, index = (index + 1) % PARALLEL_SLOTS) {
      if (!parallelSlots[index].used.exchange(true, std::memory_order_acquire)) {
        slot = &parallelSlots[index];
        break;
      }
    }
    if (slot == nullptr) {
      return nullptr;
    }

    slot->job.store(&job, std::memory_order_release);
    // Nobody is inside a slot that was free, so this is the generation alone.
    const uint64_t generation = slot->state.load(std::memory_order_acquire) >> ParallelSlot::HELPER_BITS;
    const uint64_t ticket = ticketOf(generation, index);

    size_t shared = 0;
    while (shared < helpers && parallelTickets.try_push(ticket)) {
      ++shared;
    }
    if (shared == 0) {
      // No ticket is out, the slot can go back as it is.
      slot->job.store(nullptr, std::memory_order_relaxed);
      slot->used.store(false, std::memory_order_release);
      return nullptr;
    }
    signalWork(shared);
    return slot;
  }

  bool helpParallel() {
    uint64_t ticket = 0;
    if (!parallelTickets.try_pop(ticket)) {
      return false;
    }
    take_wake_up();

    ParallelSlot &slot = parallelSlots[(ticket & ParallelSlot::HELPER_MASK) - 1];
    const uint64_t generation = ticket >> ParallelSlot::HELPER_BITS;
    // Gets in only while the slot still has the generation of the ticket. The
    // owner closes the slot on the same word, so it either sees this thread
    // inside and waits for it, or this thread sees the slot closed.
    uint64_t state = slot.state.load(std::memory_order_acquire);
    do {
      if ((state >> ParallelSlot::HELPER_BITS) != generation) {
        // Void: the loop is over, or the slot belongs to another one by now.
        return true;
      }
    } while (!slot.state.compare_exchange_weak(state, state + 1, std::memory_order_acq_rel, std::memory_order_acquire));

    ParallelJob *job = slot.job.load(std::memory_order_acquire);
    job->run(*job);
    // Nothing may touch the job after this: its owner is free to go.
    slot.state.fetch_sub(1, std::memory_order_release);
    return true;
  }

  void finishParallel(ParallelSlot &slot) {
    // The next generation, with whoever is inside kept as it is.
    uint64_t state = slot.state.load(std::memory_order_acquire);
    while (!slot.state.compare_exchange_weak(state, state + (uint64_t{1} << ParallelSlot::HELPER_BITS),
                                             std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
    // The owner has taken the last chunk before it got here, so each of them
    // only finishes the chunk it is in.
    while ((slot.state.load(std::memory_order_acquire) & ParallelSlot::HELPER_MASK) != 0) {
      std::this_thread::yield();
    }
    slot.job.store(nullptr, std::memory_order_relaxed);
    slot.used.store(false, std::memory_order_release);
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
} // namespace sc::threading::impl
