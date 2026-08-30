module;
#include <concepts>
#include <future>


export module sc.threading:dispatcher;

import :types;

import :detach;
import :enqueue;


namespace sc::threading {


  // detach
  export template<typename F, typename... Args>
    requires std::invocable<F, int, Args...> && std::is_void_v<std::invoke_result_t<F, int, Args...>>
  void detach(F &&f, Args &&...args) {
    detach<Priority::Normal>(std::forward<F>(f), std::forward<Args>(args)...);
  }

  export template<Priority P, typename F, typename... Args>
    requires std::invocable<F, int, Args...> && std::is_void_v<std::invoke_result_t<F, int, Args...>>
  void detach(F &&f, Args &&...args) {
    impl::detach<P>(std::forward<F>(f), std::forward<Args>(args)...);
  }


  export template<Priority P, std::ranges::input_range R, typename F, typename Finished, typename... Args>
    requires std::invocable<F, int, std::ranges::range_value_t<R>, Args...> &&
             std::is_void_v<std::invoke_result_t<F, int, std::ranges::range_value_t<R>, Args...>>
  void detachBatch(R &&r, F &&f, Finished &&finished, Args &&...args) {
    impl::detachBatch<P>(std::forward<R>(r), std::forward<F>(f), std::forward<Finished>(finished),
                         std::forward<Args>(args)...);
  }

  export template<std::ranges::input_range R, typename F, typename Finished, typename... Args>
    requires std::invocable<F, int, std::ranges::range_value_t<R>, Args...> &&
             std::is_void_v<std::invoke_result_t<F, int, std::ranges::range_value_t<R>, Args...>>
  void detachBatch(R &&r, F &&f, Finished &&finished, Args &&...args) {
    detachBatch<Priority::Normal>(std::forward<R>(r), std::forward<F>(f), std::forward<Finished>(finished),
                                  std::forward<Args>(args)...);
  }

  // enqueue
  export template<Priority P, typename F, typename... Args>
    requires std::invocable<F, int, Args...>
  auto enqueue(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, int, Args...>> {}

  export template<typename F, typename... Args>
    requires std::invocable<F, int, Args...>
  auto enqueue(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, int, Args...>> {
    return enqueue<Priority::Normal>(std::forward<F>(f), std::forward<Args>(args)...);
  }

  export template<Priority P, std::ranges::input_range R, typename F, typename... Args>
    requires std::invocable<F, int, std::ranges::range_value_t<R>, Args...>
  auto enqueueBatch(R &&r, F &&f, Args &&...args) {
    return impl::enqueueBatch<P>(std::forward<R>(r), std::forward<F>(f), std::forward<Args>(args)...);
  }

  export template<std::ranges::input_range R, typename F, typename... Args>
  auto enqueueBatch(R &&r, F &&f, Args &&...args) {
    return enqueueBatch<Priority::Normal>(std::forward<R>(r), std::forward<F>(f), std::forward<Args>(args)...);
  }

} // namespace sc::threading
