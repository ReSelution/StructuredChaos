#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <semaphore>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <atomic_queue/atomic_queue.h>

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
      for (auto &q: pQueues) {
        if (q.try_pop(task, threadId)) {
          return true;
        }
      }
      return false;
    }

    [[nodiscard]] bool was_empty() const {
      return std::ranges::all_of(pQueues, [](const auto &q) { return q.empty(); });
    }

    template<Priority p>
    void enqueue(MoveOnlyFunction &&task) {
      pQueues[static_cast<size_t>(p)].push(std::move(task));
    }

    template<Priority p, typename Arg>
    void emplace(Arg &&arg) {
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

  // A loop that several threads work on, see parallelFor. It lives on the
  // stack of the thread that started it, its owner.
  struct ParallelJob {
    // Works on the loop until nothing is left to take. Called by the owner
    // and by every pool thread that got into the loop with a ticket.
    void (*run)(ParallelJob &) = nullptr;
  };

  // Where a loop is open for helpers. The owner takes a slot, hands out
  // tickets for it and closes it when its own part of the loop is done. A
  // ticket names the slot and the generation the slot had when the ticket was
  // made: closing the slot moves the generation on, so a ticket that is still
  // in the queue then is void and the thread that takes it later does not
  // touch the job, which may be long gone. The owner only has to wait for the
  // threads that are inside its loop at that moment.
  struct alignas(64) ParallelSlot {
    static constexpr unsigned HELPER_BITS = 16;
    static constexpr uint64_t HELPER_MASK = (uint64_t{1} << HELPER_BITS) - 1;

    // The generation in the upper bits, below it the number of pool threads
    // that are inside the loop.
    std::atomic<uint64_t> state{0};
    std::atomic<bool> used{false};
    std::atomic<ParallelJob *> job{nullptr};
  };

  inline constexpr size_t PARALLEL_SLOTS = 1024;
  inline std::array<ParallelSlot, PARALLEL_SLOTS> parallelSlots;

  // The tickets: generation and slot, see ticketOf(). Fixed in size and never
  // freed, which is fine for a few numbers. A loop that finds it full gets
  // fewer helpers.
  inline atomic_queue::AtomicQueue<uint64_t, PARALLEL_SLOTS> parallelTickets;

  // Opens a slot for the job, hands out up to the given number of tickets, at
  // most one for every pool thread, and wakes the threads up. Null if no
  // ticket was handed out; there is nothing to finish then.
  [[nodiscard]] ParallelSlot *shareParallel(ParallelJob &job, size_t helpers);

  // Takes a ticket, if there is one, and works on its loop unless the ticket
  // is void. Only for threads that have nothing else to do.
  bool helpParallel();

  // See sc::threading::helpThread().
  bool help_impl();

  // Closes the slot, which makes the tickets nobody has taken void, and
  // returns once the threads that are inside the loop have left it. Does not
  // work on anything in the meantime: what is left of the loop is the chunk
  // each of those threads is in the middle of.
  void finishParallel(ParallelSlot &slot);

  void workerThread(const std::stop_token &st, int id);

  void signalWork(size_t count);

  void shutdown();

  void init_impl(uint32_t numThreads = std::thread::hardware_concurrency() - 1);

  void wait_until_finished();

} // namespace sc::threading::impl
