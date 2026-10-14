#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "internal.hpp"
#include "ring_queue.hpp"
#include "types.hpp"

// The pool behind sc::threading. Its state lives in backend.cpp; only what the
// templates of enqueue, detach and parallelFor need is declared here.
namespace sc::threading::internal {

  using TaskQueue = RingQueue<MoveOnlyFunction, QUEUE_CAP>;

  // Counts a task as outstanding. Has to be called before the task is pushed:
  // a worker may pop and finish it right away, and it must not find the
  // counter without that task.
  void onTaskEnqueued() noexcept;

  // Wakes up to count workers.
  void signalWork(size_t count);

  // Logs the exception that is being handled. For the catch block of a task
  // nobody waits for.
  void logDetachedException() noexcept;

  struct Queues {
    std::array<TaskQueue, PRIORITY_COUNT> byPriority;

    bool tryPopAny(MoveOnlyFunction &task, int32_t workerId) {
      return std::ranges::any_of(byPriority, [&](TaskQueue &q) { return q.tryPop(task, workerId); });
    }

    [[nodiscard]] bool empty() const {
      return std::ranges::all_of(byPriority, [](const TaskQueue &q) { return q.empty(); });
    }

    // Builds the task first, which may throw, and only then counts and pushes
    // it, which does not. A failed task leaves no trace in the pool.
    template<Priority P, typename Callable>
    void submit(Callable &&callable) {
      MoveOnlyFunction task(std::forward<Callable>(callable));
      onTaskEnqueued();
      byPriority[static_cast<size_t>(P)].push(std::move(task));
    }
  };

  extern Queues queues;

  // 0 for the thread that called init and for threads outside the pool, a
  // positive number for pool workers.
  extern constinit thread_local int32_t currentThreadId;

  // Asked by a helper before it takes the next chunk of a loop, see
  // sc::threading::helpThread(stop). Null for whoever does not stop early.
  using StopCheck = bool (*)(void *context) noexcept;

  // A loop that several threads work on, see parallelFor. It lives on the
  // stack of the thread that started it, its owner.
  struct ParallelJob {
    // Works on the loop until nothing is left to take, or until stop says so
    // before the next chunk. Called by the owner, which never stops early,
    // and by every thread that got into the loop with a ticket.
    void (*run)(ParallelJob &, StopCheck stop, void *context) = nullptr;
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

  // Opens a slot for the job, hands out up to the given number of tickets, at
  // most one for every pool thread, and wakes the threads up. Null if no
  // ticket was handed out; there is nothing to finish then. With ticket the
  // slot is opened in any case, unless all of them are taken, and its ticket
  // is written there for threads that join the loop by it.
  [[nodiscard]] ParallelSlot *shareParallel(ParallelJob &job, size_t helpers, uint64_t *ticket = nullptr);

  // Takes a ticket, if there is one, and works on its loop unless the ticket
  // is void. Only for threads that have nothing else to do. With a stop
  // check it leaves the loop before the first chunk for which stop is true.
  bool helpParallel(StopCheck stop = nullptr, void *context = nullptr);

  // Works on the loop of the ticket like helpParallel, without taking a
  // ticket from the queue. False if the ticket is void.
  bool joinParallel(uint64_t ticket, StopCheck stop = nullptr, void *context = nullptr);

  // The StopCheck for a predicate, which is passed as the context.
  template<typename S>
  bool stopCheckOf(void *context) noexcept {
    return static_cast<bool>((*static_cast<S *>(context))());
  }

  // Closes the slot, which makes the tickets nobody has taken void, and
  // returns once the threads that are inside the loop have left it. Does not
  // work on anything in the meantime: what is left of the loop is the chunk
  // each of those threads is in the middle of.
  void finishParallel(ParallelSlot &slot);

  void initPool(uint32_t requested);

  void shutdownPool();

  void waitUntilFinished();

  // One more than the highest thread id there is.
  [[nodiscard]] size_t threadSlots() noexcept;

} // namespace sc::threading::internal
