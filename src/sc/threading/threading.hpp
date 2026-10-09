#pragma once

#include <concepts>
#include <cstdint>
#include <future>
#include <thread>

#include "detach.hpp"
#include "enqueue.hpp"
#include "parallel_for.hpp"
#include "types.hpp"
namespace sc::threading {

  void init(uint32_t threads = std::thread::hardware_concurrency() - 1);

  void wait_until_finished();

  /// @brief Id of the calling thread: 0 for the thread that called init and
  /// for threads outside the pool, a positive number for pool workers.
  [[nodiscard]] inline int32_t thread_id() noexcept { return impl::threadId; }

  /// @brief One more than the highest thread_id there is: the size of an
  /// array that has an entry for every thread. Changes with init.
  [[nodiscard]] size_t thread_slots() noexcept;

  // Calls function(first, last) for the items 0 to count - 1 in chunks of at
  // most grain items, on the calling thread and on every pool thread that has
  // nothing else to do, and returns once all of them are done.
  //
  // The function is called from several threads at once. May be called from
  // anywhere, also from a task of the pool and from inside another
  // parallelFor: the calling thread never sleeps, it works on the loop itself
  // and, while it waits for the last chunks, on the loops of others.
  //
  // If the function throws, no further chunk is started and the first
  // exception is rethrown once the running ones are done.
  template <typename F>
    requires std::invocable<F &, size_t, size_t>
  void parallelFor(size_t count, size_t grain, F &&function) {
    impl::parallelFor(count, grain, SIZE_MAX, function);
  }

  // The same with at most the given number of pool threads helping, for a
  // loop that knows it has too little work for all of them: waking a thread
  // up costs more than a small chunk. With no helpers the calling thread does
  // everything itself.
  template <typename F>
    requires std::invocable<F &, size_t, size_t>
  void parallelFor(size_t count, size_t grain, size_t helpers, F &&function) {
    impl::parallelFor(count, grain, helpers, function);
  }

  // detach
  template<Priority P, typename F, typename... Args>
    requires std::invocable<F, Args...> && std::is_void_v<std::invoke_result_t<F, Args...>>
  void detach(F &&f, Args &&...args) {
    impl::detach<P>(std::forward<F>(f), std::forward<Args>(args)...);
  }

  template<typename F, typename... Args>
    requires std::invocable<F, Args...> && std::is_void_v<std::invoke_result_t<F, Args...>>
  void detach(F &&f, Args &&...args) {
    detach<Priority::Normal>(std::forward<F>(f), std::forward<Args>(args)...);
  }

  template<Priority P, std::ranges::input_range R, typename F, typename Finished, typename... Args>
    requires std::invocable<F, std::ranges::range_value_t<R>, Args...> &&
             std::is_void_v<std::invoke_result_t<F, std::ranges::range_value_t<R>, Args...>>
  void detachBatch(R &&r, F &&f, Finished &&finished, Args &&...args) {
    impl::detachBatch<P>(std::forward<R>(r), std::forward<F>(f), std::forward<Finished>(finished),
                         std::forward<Args>(args)...);
  }

  template<std::ranges::input_range R, typename F, typename Finished, typename... Args>
    requires std::invocable<F, std::ranges::range_value_t<R>, Args...> &&
             std::is_void_v<std::invoke_result_t<F, std::ranges::range_value_t<R>, Args...>>
  void detachBatch(R &&r, F &&f, Finished &&finished, Args &&...args) {
    detachBatch<Priority::Normal>(std::forward<R>(r), std::forward<F>(f), std::forward<Finished>(finished),
                                  std::forward<Args>(args)...);
  }

  // enqueue
  template<Priority P, typename F, typename... Args>
    requires std::invocable<F, Args...>
  auto enqueue(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
    return impl::enqueue<P>(std::forward<F>(f), std::forward<Args>(args)...);
  }

  template<typename F, typename... Args>
    requires std::invocable<F, Args...>
  auto enqueue(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
    return enqueue<Priority::Normal>(std::forward<F>(f), std::forward<Args>(args)...);
  }

  template<Priority P, std::ranges::input_range R, typename F, typename... Args>
    requires std::invocable<F, std::ranges::range_value_t<R>, Args...>
  auto enqueueBatch(R &&r, F &&f, Args &&...args) {
    return impl::enqueueBatch<P>(std::forward<R>(r), std::forward<F>(f), std::forward<Args>(args)...);
  }

  template<std::ranges::input_range R, typename F, typename... Args>
  auto enqueueBatch(R &&r, F &&f, Args &&...args) {
    return enqueueBatch<Priority::Normal>(std::forward<R>(r), std::forward<F>(f), std::forward<Args>(args)...);
  }

} // namespace sc::threading
