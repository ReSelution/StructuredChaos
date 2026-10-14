#pragma once

#include <concepts>
#include <cstdint>
#include <future>
#include <memory>
#include <ranges>
#include <type_traits>

#include "detach.hpp"
#include "enqueue.hpp"
#include "parallel_for.hpp"
#include "types.hpp"

namespace sc::threading {

  // One worker less than the hardware has threads, at least one.
  [[nodiscard]] uint32_t defaultWorkerCount() noexcept;

  // Starts the pool with the given number of workers, at most one less than
  // the hardware has threads. Does nothing if it is running already.
  void init(uint32_t threads = defaultWorkerCount());

  // Stops and joins the workers. Tasks that have not started are dropped: a
  // future of enqueue then throws std::future_error. Runs on its own at exit.
  void shutdown();

  // Returns once every task that was enqueued or detached has finished.
  void waitUntilFinished();

  // Joins a parallelFor loop of another thread that has room for helpers, if
  // there is one, works on it until nothing of it is left to take and returns
  // whether there was one. Tasks of the queues are left to the pool.
  //
  // For a thread that waits for something another thread does and would
  // otherwise only yield, typically for what that thread loads in a
  // parallelFor. Before init there is nothing to help.
  bool helpThread();

  // The same, but before every chunk it would take next, the first one
  // included, stop() is asked, and once it is true the thread leaves the loop:
  // the chunk it is in is always finished, the rest is left to the others.
  // For a wait that is over long before the loop: helpThread([&] { return
  // ready.load(); }). Returns true also if stop was true right away.
  //
  // stop is called on this thread only, but in the middle of the loop of
  // another one, so it must not throw: an exception ends the program.
  template<typename Stop>
    requires std::predicate<Stop &>
  bool helpThread(Stop &&stop) {
    return internal::helpParallel(internal::stopCheckOf<std::remove_reference_t<Stop>>,
                                  const_cast<void *>(static_cast<const void *>(std::addressof(stop))));
  }

  // Joins exactly the loop of the ticket, which parallelFor with onTicket
  // gave out, and works on it like helpThread() does. Only that loop, so a
  // thread can help with what it waits for without getting into work of
  // somebody else that may wait for it in turn. False if the ticket is void,
  // because the loop is over or there was none.
  bool helpThread(LoopTicket ticket);

  // The same with stop, as for helpThread(stop). Returns true also if stop
  // was true right away.
  template<typename Stop>
    requires std::predicate<Stop &>
  bool helpThread(LoopTicket ticket, Stop &&stop) {
    return internal::joinParallel(ticket.value, internal::stopCheckOf<std::remove_reference_t<Stop>>,
                                  const_cast<void *>(static_cast<const void *>(std::addressof(stop))));
  }

  // Id of the calling thread: 0 for the thread that called init and for
  // threads outside the pool, a positive number for pool workers.
  [[nodiscard]] inline int32_t threadId() noexcept { return internal::currentThreadId; }

  // One more than the highest threadId there is: the size of an array that
  // has an entry for every thread. Changes with init.
  [[nodiscard]] size_t threadSlots() noexcept;

  // Calls function(first, last) for the items 0 to count - 1 in chunks of at
  // most grain items, on the calling thread and on every pool thread that has
  // nothing else to do, and returns once all of them are done.
  //
  // The function is called from several threads at once. May be called from
  // anywhere, also from a task of the pool and from inside another
  // parallelFor: the calling thread works on the loop itself and does not
  // depend on anybody joining in. If no pool thread is free, it does all of
  // it alone. Once nothing of the loop is left to take, it only waits for the
  // pool threads that are in the middle of a chunk of it; it never works on
  // the loop of somebody else, that is left to threads with nothing to do.
  //
  // If the function throws, no further chunk is started and the first
  // exception is rethrown once the running ones are done.
  template<typename F>
    requires std::invocable<F &, size_t, size_t>
  void parallelFor(size_t count, size_t grain, F &&function) {
    internal::parallelFor(count, grain, SIZE_MAX, function);
  }

  // The same with at most the given number of pool threads helping, for a
  // loop that knows it has too little work for all of them: waking a thread
  // up costs more than a small chunk. With no helpers the calling thread does
  // everything itself.
  template<typename F>
    requires std::invocable<F &, size_t, size_t>
  void parallelFor(size_t count, size_t grain, size_t helpers, F &&function) {
    internal::parallelFor(count, grain, helpers, function);
  }

  // The same, and before the calling thread starts on the loop, onTicket is
  // called with its ticket, for threads that wait for the loop to join it by
  // helpThread(ticket). They can join it even with no helpers. Not called if
  // the loop is a single chunk, or if all slots for shared loops are taken.
  // The ticket turns void when parallelFor returns. If onTicket throws, no
  // chunk is started and the exception is passed on.
  template<typename F, typename OnTicket>
    requires std::invocable<F &, size_t, size_t> && std::invocable<OnTicket &, LoopTicket>
  void parallelFor(size_t count, size_t grain, size_t helpers, F &&function, OnTicket &&onTicket) {
    internal::parallelFor(count, grain, helpers, function, onTicket);
  }

  // Runs the function on the pool. Nobody waits for it; an exception it
  // throws is logged and dropped.
  template<Priority P, typename F, typename... Args>
    requires std::invocable<F, Args...> && std::is_void_v<std::invoke_result_t<F, Args...>>
  void detach(F &&f, Args &&...args) {
    internal::detach<P>(std::forward<F>(f), std::forward<Args>(args)...);
  }

  template<typename F, typename... Args>
    requires std::invocable<F, Args...> && std::is_void_v<std::invoke_result_t<F, Args...>>
  void detach(F &&f, Args &&...args) {
    detach<Priority::Normal>(std::forward<F>(f), std::forward<Args>(args)...);
  }

  // One task per element of the range; finished (or nullptr) is called once
  // the last of them is done.
  template<Priority P, std::ranges::sized_range R, typename F, typename Finished, typename... Args>
    requires std::invocable<F, std::ranges::range_value_t<R>, Args...> &&
             std::is_void_v<std::invoke_result_t<F, std::ranges::range_value_t<R>, Args...>>
  void detachBatch(R &&r, F &&f, Finished &&finished, Args &&...args) {
    internal::detachBatch<P>(std::forward<R>(r), std::forward<F>(f), std::forward<Finished>(finished),
                             std::forward<Args>(args)...);
  }

  template<std::ranges::sized_range R, typename F, typename Finished, typename... Args>
    requires std::invocable<F, std::ranges::range_value_t<R>, Args...> &&
             std::is_void_v<std::invoke_result_t<F, std::ranges::range_value_t<R>, Args...>>
  void detachBatch(R &&r, F &&f, Finished &&finished, Args &&...args) {
    detachBatch<Priority::Normal>(std::forward<R>(r), std::forward<F>(f), std::forward<Finished>(finished),
                                  std::forward<Args>(args)...);
  }

  // Runs the function on the pool; the future gets its result or exception.
  template<Priority P, typename F, typename... Args>
    requires std::invocable<F, Args...>
  auto enqueue(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
    return internal::enqueue<P>(std::forward<F>(f), std::forward<Args>(args)...);
  }

  template<typename F, typename... Args>
    requires std::invocable<F, Args...>
  auto enqueue(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
    return enqueue<Priority::Normal>(std::forward<F>(f), std::forward<Args>(args)...);
  }

  // One task per element of the range. Returns a std::future<void> for a
  // function without result, otherwise a FutureGroup with one future per
  // element, in the order of the range.
  template<Priority P, std::ranges::sized_range R, typename F, typename... Args>
    requires std::invocable<F, std::ranges::range_value_t<R>, Args...>
  auto enqueueBatch(R &&r, F &&f, Args &&...args) {
    return internal::enqueueBatch<P>(std::forward<R>(r), std::forward<F>(f), std::forward<Args>(args)...);
  }

  template<std::ranges::sized_range R, typename F, typename... Args>
    requires std::invocable<F, std::ranges::range_value_t<R>, Args...>
  auto enqueueBatch(R &&r, F &&f, Args &&...args) {
    return enqueueBatch<Priority::Normal>(std::forward<R>(r), std::forward<F>(f), std::forward<Args>(args)...);
  }

} // namespace sc::threading
