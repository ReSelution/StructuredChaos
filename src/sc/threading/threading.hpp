#pragma once

#include <concepts>
#include <cstdint>
#include <future>
#include <thread>

#include "detach.hpp"
#include "enqueue.hpp"
#include "types.hpp"
namespace sc::threading {

  void init(uint32_t threads = std::thread::hardware_concurrency() - 1);

  void wait_until_finished();

  /// @brief Id of the calling thread: 0 for the thread that called init and
  /// for threads outside the pool, a positive number for pool workers.
  [[nodiscard]] inline int32_t thread_id() noexcept { return impl::threadId; }

  // detach
  template <Priority P, typename F, typename... Args>
    requires std::invocable<F, Args...> && std::is_void_v<std::invoke_result_t<F, Args...>>
  void detach(F &&f, Args &&...args) {
    impl::detach<P>(std::forward<F>(f), std::forward<Args>(args)...);
  }

  template <typename F, typename... Args>
    requires std::invocable<F, Args...> && std::is_void_v<std::invoke_result_t<F, Args...>>
  void detach(F &&f, Args &&...args) {
    detach<Priority::Normal>(std::forward<F>(f), std::forward<Args>(args)...);
  }

  template <Priority P, std::ranges::input_range R, typename F, typename Finished, typename... Args>
    requires std::invocable<F, std::ranges::range_value_t<R>, Args...> &&
             std::is_void_v<std::invoke_result_t<F, std::ranges::range_value_t<R>, Args...>>
  void detachBatch(R &&r, F &&f, Finished &&finished, Args &&...args) {
    impl::detachBatch<P>(std::forward<R>(r), std::forward<F>(f), std::forward<Finished>(finished),
                         std::forward<Args>(args)...);
  }

  template <std::ranges::input_range R, typename F, typename Finished, typename... Args>
    requires std::invocable<F, std::ranges::range_value_t<R>, Args...> &&
             std::is_void_v<std::invoke_result_t<F, std::ranges::range_value_t<R>, Args...>>
  void detachBatch(R &&r, F &&f, Finished &&finished, Args &&...args) {
    detachBatch<Priority::Normal>(std::forward<R>(r), std::forward<F>(f), std::forward<Finished>(finished),
                                  std::forward<Args>(args)...);
  }

  // enqueue
  template <Priority P, typename F, typename... Args>
    requires std::invocable<F, Args...>
  auto enqueue(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
    return impl::enqueue<P>(std::forward<F>(f), std::forward<Args>(args)...);
  }

  template <typename F, typename... Args>
    requires std::invocable<F, Args...>
  auto enqueue(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
    return enqueue<Priority::Normal>(std::forward<F>(f), std::forward<Args>(args)...);
  }

  template <Priority P, std::ranges::input_range R, typename F, typename... Args>
    requires std::invocable<F, std::ranges::range_value_t<R>, Args...>
  auto enqueueBatch(R &&r, F &&f, Args &&...args) {
    return impl::enqueueBatch<P>(std::forward<R>(r), std::forward<F>(f), std::forward<Args>(args)...);
  }

  template <std::ranges::input_range R, typename F, typename... Args> auto enqueueBatch(R &&r, F &&f, Args &&...args) {
    return enqueueBatch<Priority::Normal>(std::forward<R>(r), std::forward<F>(f), std::forward<Args>(args)...);
  }

} // namespace sc::threading
