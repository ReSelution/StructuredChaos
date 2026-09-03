module;

#include <concepts>
#include <cstddef>
#include <memory>
#include <ranges>
export module sc.threading:detach;

import :types;
import :internal;
import :backend;

namespace sc::threading::impl {


  export template<Priority P, typename F, typename... Args>
    requires std::invocable<F, int, Args...> && std::is_void_v<std::invoke_result_t<F, int, Args...>>
  void detach(F &&f, Args &&...args) {
    constexpr size_t SizeF        = sizeof(std::decay_t<F>);
    constexpr size_t SizeArgs     = (sizeof(std::decay_t<Args>) + ... + 0);
    constexpr size_t MIN_OVERHEAD = 8;

    if constexpr (SizeF + SizeArgs + MIN_OVERHEAD <= SFO_LIMIT) {
      pushTask<P>([f = std::forward<F>(f), ... args = std::forward<Args>(args)](int id) mutable {
        try {
          f(id, std::forward<Args>(args)...);
        } catch (...) {
        }
      });
    } else {
      auto ctx = std::make_unique<std::tuple<std::decay_t<F>, std::tuple<std::decay_t<Args>...>>>(
          std::forward<F>(f), std::make_tuple(std::forward<Args>(args)...));
      pushTask<P>([ctx = std::move(ctx)](int id) mutable {
        auto &func      = std::get<0>(*ctx);
        auto &base_args = std::get<1>(*ctx);
        std::apply([&](auto &&...unpacked) { func(id, std::forward<decltype(unpacked)>(unpacked)...); }, base_args);
      });
    }
  }

  // Case 1: Fast Path OHNE Callback (Finished == nullptr_t)
  template<Priority P, typename R, typename F, typename... Args>
  void detachBatchSfoNoCallback(R &&r, F &&f, Args &&...args) {
    ThreadLog::info("SFO no Callback");
    for (auto &&item: r) {
      queues.emplace<P>([f, args..., arg = std::move(item)](int id) mutable {
        try {
          f(id, std::move(arg), args...);
        } catch (const std::exception &e) {
          ThreadLog::err("Exception: {}", e.what());
        }
      });
    }
  }

  // Case 2: Fast Path MIT Callback
  template<Priority P, typename R, typename F, typename Finished, typename... Args>
  void detachBatchSfoWithCallback(R &&r, F &&f, Finished &&finished, size_t count, Args &&...args) {

    auto state = std::make_shared<DetachBatchState>(count, std::forward<Finished>(finished));

    for (auto &&item: r) {
      queues.emplace<P>([f, args..., arg = std::move(item), state](int id) mutable {
        auto invoke_finished_if_last = [&]() {
          if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (state->on_finished) {
              state->on_finished(id);
            }
          }
        };

        try {
          f(id, std::move(arg), args...);
          invoke_finished_if_last();
        } catch (...) {
          invoke_finished_if_last();
        }
      });
    }
  }

  // =========================================================================
  // HEAP PATH: Daten werden in einem MergedState auf den Heap ausgelagert
  // =========================================================================

  // Case 3: Heap Path OHNE Callback (Finished == nullptr_t)
  template<Priority P, typename R, typename F, typename... Args>
  void detachBatchHeapNoCallback(R &&r, F &&f, size_t count, Args &&...args) {

    auto shared =
        std::make_shared<DetachedMergedStateNoCallback<R, Args...>>(std::forward<R>(r), std::forward<Args>(args)...);

    for (size_t i = 0; i < count; ++i) {
      queues.emplace<P>([f, shared, i](int id) {
        try {
          std::apply([&](auto &&...unpacked) { f(id, std::move(shared->payload[i]), unpacked...); },
                     shared->saved_args);
        } catch (...) {
        }
      });
    }
  }

  // Case 4: Heap Path MIT Callback
  template<Priority P, typename R, typename F, typename Finished, typename... Args>
  void detachBatchHeapWithCallback(R &&r, F &&f, Finished &&finished, size_t count, Args &&...args) {


    auto shared = std::make_shared<DetachBatchState>(count, std::forward<R>(r), std::forward<Finished>(finished),
                                                     std::forward<Args>(args)...);

    for (size_t i = 0; i < count; ++i) {
      queues.emplace<P>([f, shared, i](int id) {
        auto invoke_finished_if_last = [&]() {
          if (shared->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (shared->on_finished) {
              shared->on_finished(id);
            }
          }
        };

        try {
          std::apply([&](auto &&...unpacked) { f(id, std::move(shared->payload[i]), unpacked...); },
                     shared->saved_args);
          invoke_finished_if_last();
        } catch (...) {
          invoke_finished_if_last();
        }
      });
    }
  }

  export template<Priority P, std::ranges::sized_range R, typename F, typename Finished, typename... Args>
    requires std::invocable<F, int, std::ranges::range_value_t<R>, Args...> &&
             std::is_void_v<std::invoke_result_t<F, int, std::ranges::range_value_t<R>, Args...>>
  void detachBatch(R &&r, F &&f, Finished &&finished, Args &&...args) {
    const auto count = std::ranges::size(r);
    if (count == 0) {
      return;
    }

    using ArgType = std::ranges::range_value_t<R>;

    constexpr size_t CAPTURE_BASE = sizeof(std::decay_t<F>) + (0 + ... + sizeof(std::decay_t<Args>)) + sizeof(ArgType);
    constexpr size_t OVERHEAD     = sizeof(std::shared_ptr<void>);

    constexpr bool is_null_type = std::is_same_v<std::remove_cvref_t<Finished>, std::nullptr_t>;
    constexpr bool fits_sfo     = CAPTURE_BASE + OVERHEAD <= SFO_LIMIT || (!is_null_type && CAPTURE_BASE <= SFO_LIMIT);

    if constexpr (fits_sfo) {
      if constexpr (is_null_type) {
        detachBatchSfoNoCallback<P>(std::forward<R>(r), std::forward<F>(f), std::forward<Args>(args)...);
      } else {
        detachBatchSfoWithCallback<P>(std::forward<R>(r), std::forward<F>(f), std::forward<Finished>(finished), count,
                                      std::forward<Args>(args)...);
      }
    } else {
      if constexpr (is_null_type) {
        detachBatchHeapNoCallback<P>(std::forward<R>(r), std::forward<F>(f), count, std::forward<Args>(args)...);
      } else {
        detachBatchHeapWithCallback<P>(std::forward<R>(r), std::forward<F>(f), std::forward<Finished>(finished), count,
                                       std::forward<Args>(args)...);
      }
    }

    QueueSize::record(count);
    signalWork(count);
  }

} // namespace sc::threading::impl
