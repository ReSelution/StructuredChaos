#include "backend.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <semaphore>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <atomic_queue/atomic_queue.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cwchar>

#include <windows.h>
#elifdef __linux__
#include <pthread.h>
#endif

namespace sc::threading::internal {

  Queue queue;
  constinit thread_local uint32_t currentThreadId = NO_THREAD_ID;

  namespace {
    // Guards the worker list in init and shutdown.
    std::mutex threadMutex;
    std::vector<std::jthread> threads;
    // threads.size(), for readers that must not take the mutex.
    std::atomic<size_t> workerCount{0};

    inline constexpr size_t MAX_TASKS = QUEUE_CAP;
    std::counting_semaphore<MAX_TASKS> poolSemaphore{0};

    // Tasks that were enqueued and have not finished yet, and the threads that
    // wait for them in waitUntilFinished.
    std::mutex waitMutex;
    std::condition_variable waitCondition;
    std::atomic<size_t> outstandingTasks{0};
    std::atomic<size_t> idleWaiters{0};

    inline constexpr size_t PARALLEL_SLOTS = 1024;
    std::array<ParallelSlot, PARALLEL_SLOTS> parallelSlots;

    // The tickets: generation and slot, see ticketOf(). Fixed in size and never
    // freed, which is fine for a few numbers. A loop that finds it full gets
    // fewer helpers.
    atomic_queue::AtomicQueue<uint64_t, PARALLEL_SLOTS> parallelTickets;

    // The tickets of loops of the background lane, see helpBackground.
    atomic_queue::AtomicQueue<uint64_t, PARALLEL_SLOTS> backgroundTickets;

    // Whether this thread was woken up for work it has not found yet.
    thread_local bool wokenForWork = false;

    // Every piece of work comes with one wake-up. A thread that was woken up
    // for it has used that up already; one that found the work while it was
    // awake anyway takes the wake-up back, so that nobody gets up for nothing.
    void takeWakeUp() {
      if (wokenForWork) {
        wokenForWork = false;
      } else {
        // Nothing to take back if the woken thread was faster.
        (void) poolSemaphore.try_acquire();
      }
    }

    void onTaskStarted() {
      takeWakeUp();
      ActiveTask::record(1);
      QueueSize::record(static_cast<uint64_t>(-1));
    }

    // The counter and the waiter count are read and written with the default
    // sequentially consistent ordering on purpose: either the waiter sees the
    // counter at zero, or the finishing thread sees the waiter and wakes it.
    void onTasksFinished(size_t count = 1) {
      if (outstandingTasks.fetch_sub(count) == count && idleWaiters.load() != 0) {
        // Taking the mutex keeps the notification from slipping in between the
        // waiter's check of the counter and its going to sleep.
        {
          std::scoped_lock lock(waitMutex);
        }
        waitCondition.notify_all();
      }
    }

    void runTask(MoveOnlyFunction &task) {
      onTaskStarted();
      // The task wrappers of enqueue and detach catch everything themselves.
      // This is the last line of defense: an exception that escaped would end
      // the program through the worker thread.
      try {
        task();
      } catch (const std::exception &e) {
        ThreadLog::err("A task threw: {}", e.what());
      } catch (...) {
        ThreadLog::err("A task threw an exception that is not a std::exception");
      }
      // Destroyed before the task counts as finished: whoever waits for it may
      // rely on everything it captured being gone.
      task = nullptr;
      ActiveTask::record(static_cast<uint64_t>(-1));
      onTasksFinished();
    }

    // Whether the frame has work for a worker: a loop or a task. Asked before
    // every chunk of a loop of the lane.
    bool frameWaiting(void *context) noexcept {
      const bool waiting = !parallelTickets.was_empty() || !queue.empty();
      if (waiting) {
        *static_cast<bool *>(context) = true;
      }
      return waiting;
    }

    // Takes a ticket of a loop of the lane and works on it until nothing of
    // it is left or the frame has work, which it then gets after the chunk
    // in hand: the frame waits for at most one chunk of the lane per worker
    // that is inside. A worker that left early puts the ticket back, so that
    // the loop gets its helpers again once the frame is done.
    bool helpBackground() {
      uint64_t ticket = 0;
      if (!backgroundTickets.try_pop(ticket)) {
        return false;
      }
      takeWakeUp();
      bool leftForFrame = false;
      if (joinParallel(ticket, frameWaiting, &leftForFrame) && leftForFrame) {
        // No wake-up with it: the workers are awake for the frame and look
        // here again before they sleep. A full queue loses a helper.
        (void) backgroundTickets.try_push(ticket);
      }
      return true;
    }

    void workerThread(const std::stop_token &st, uint32_t id) {
      currentThreadId = id;
      setThreadName("sc-worker-" + std::to_string(id));
      while (!st.stop_requested()) {
        // First: a thread is waiting for these to be done.
        if (helpParallel()) {
          continue;
        }
        MoveOnlyFunction task;
        if (queue.tryPop(task, static_cast<int32_t>(currentThreadId))) {
          runTask(task);
          continue;
        }
        // Last: the frame comes first.
        if (helpBackground()) {
          continue;
        }

        wokenForWork = poolSemaphore.try_acquire_for(std::chrono::milliseconds(10));
      }
    }

    // Never 0: that is what the queue keeps in an empty place.
    constexpr uint64_t ticketOf(uint64_t generation, size_t slot) {
      return (generation << ParallelSlot::HELPER_BITS) | (slot + 1);
    }
  } // namespace

  void setThreadName(std::string_view name) {
#ifdef _WIN32
    // A string_view is not necessarily terminated, so its length is passed on.
    std::array<wchar_t, 64> wideName{};
    std::swprintf(wideName.data(), wideName.size(), L"%.*hs", static_cast<int>(name.size()), name.data());
    SetThreadDescription(GetCurrentThread(), wideName.data());
#elifdef __linux__
    // Linux allows 15 characters plus the terminator.
    std::array<char, 16> shortName{};
    name.copy(shortName.data(), shortName.size() - 1);
    pthread_setname_np(pthread_self(), shortName.data());
#else
    (void) name;
#endif
  }

  void onTaskEnqueued() noexcept {
    outstandingTasks.fetch_add(1, std::memory_order_relaxed);
    QueueSize::record(1);
  }

  void logDetachedException() noexcept {
    try {
      try {
        throw;
      } catch (const std::exception &e) {
        ThreadLog::err("A detached task threw: {}", e.what());
      } catch (...) {
        ThreadLog::err("A detached task threw an exception that is not a std::exception");
      }
    } catch (...) { // NOLINT(bugprone-empty-catch)
      // Logging failed as well; there is nobody left to tell.
    }
  }

  void signalWork(size_t count) { poolSemaphore.release(static_cast<std::ptrdiff_t>(count)); }

  void waitUntilFinished() {
    idleWaiters.fetch_add(1);
    {
      std::unique_lock lock(waitMutex);
      waitCondition.wait(lock, [] { return outstandingTasks.load() == 0; });
    }
    idleWaiters.fetch_sub(1);
  }

  void initPool(uint32_t requested) {
    std::scoped_lock lock(threadMutex);
    if (!threads.empty()) [[unlikely]] {
      return;
    }

    // hardware_concurrency() may report 0 if it does not know.
    const size_t hardware = std::max<size_t>(std::thread::hardware_concurrency(), 2);
    const size_t threadCount = std::min<size_t>(requested, hardware - 1);

    static std::once_flag atexitRegistered;
    std::call_once(atexitRegistered, [] { std::atexit(shutdownPool); });

    threads.reserve(threadCount);
    for (size_t i = 0; i < threadCount; ++i) {
      threads.emplace_back(workerThread, static_cast<uint32_t>(i + 1));
    }
    workerCount.store(threads.size(), std::memory_order_release);
    currentThreadId = 0;
  }

  void shutdownPool() {
    std::vector<std::jthread> stopping;
    {
      std::scoped_lock lock(threadMutex);
      for (auto &t: threads) {
        t.request_stop();
      }
      stopping.swap(threads);
      workerCount.store(0, std::memory_order_release);
    }
    signalWork(stopping.size());
    // Joins every worker.
    stopping.clear();

    // What nobody runs any more is dropped. A task of enqueue then breaks its
    // promise, so a waiting future throws std::future_error instead of
    // blocking, and waitUntilFinished returns.
    MoveOnlyFunction task;
    size_t dropped = 0;
    while (queue.tryPop(task, 0)) {
      task = nullptr;
      ++dropped;
    }
    if (dropped != 0) {
      QueueSize::record(static_cast<uint64_t>(0) - dropped);
      onTasksFinished(dropped);
    }
  }

  size_t threadSlots() noexcept { return 1 + workerCount.load(std::memory_order_acquire); }

  ParallelSlot *shareParallel(ParallelJob &job, size_t helpers, uint64_t *ticketOut, bool background) {
    helpers = std::min(helpers, workerCount.load(std::memory_order_acquire));
    if (helpers == 0 && ticketOut == nullptr) {
      return nullptr;
    }

    // Every thread starts at a place of its own, so that loops of different
    // threads do not meet in the same slots.
    ParallelSlot *slot = nullptr;
    size_t index = static_cast<size_t>(currentThreadId) % PARALLEL_SLOTS;
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

    auto &tickets = background ? backgroundTickets : parallelTickets;
    size_t shared = 0;
    while (shared < helpers && tickets.try_push(ticket)) {
      ++shared;
    }
    if (ticketOut != nullptr) {
      *ticketOut = ticket;
    } else if (shared == 0) {
      // No ticket is out, the slot can go back as it is.
      slot->job.store(nullptr, std::memory_order_relaxed);
      slot->used.store(false, std::memory_order_release);
      return nullptr;
    }
    if (shared != 0) {
      signalWork(shared);
    }
    return slot;
  }

  bool helpParallel(StopCheck stop, void *context) {
    uint64_t ticket = 0;
    if (!parallelTickets.try_pop(ticket)) {
      return false;
    }
    takeWakeUp();
    joinParallel(ticket, stop, context);
    return true;
  }

  bool joinParallel(uint64_t ticket, StopCheck stop, void *context) {
    const uint64_t index = ticket & ParallelSlot::HELPER_MASK;
    if (index == 0 || index > PARALLEL_SLOTS) {
      return false;
    }
    ParallelSlot &slot = parallelSlots[index - 1];
    const uint64_t generation = ticket >> ParallelSlot::HELPER_BITS;
    // Gets in only while the slot still has the generation of the ticket. The
    // owner closes the slot on the same word, so it either sees this thread
    // inside and waits for it, or this thread sees the slot closed.
    uint64_t state = slot.state.load(std::memory_order_acquire);
    do {
      if ((state >> ParallelSlot::HELPER_BITS) != generation) {
        // Void: the loop is over, or the slot belongs to another one by now.
        return false;
      }
    } while (!slot.state.compare_exchange_weak(state, state + 1, std::memory_order_acq_rel, std::memory_order_acquire));

    ParallelJob *job = slot.job.load(std::memory_order_acquire);
    job->run(*job, stop, context);
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
} // namespace sc::threading::internal
