#pragma once

#include <concepts>
#include <cstddef>
#include <memory>
#include <ranges>

#include "backend.hpp"
#include "internal.hpp"
#include "types.hpp"

namespace sc::threading::impl {

  template <Priority P, typename F, typename... Args>
    requires std::invocable<F, Args...> && std::is_void_v<std::invoke_result_t<F, Args...>>
  void detach(F &&f, Args &&...args) {
    constexpr size_t SizeF = sizeof(std::decay_t<F>);
    constexpr size_t SizeArgs = (sizeof(std::decay_t<Args>) + ... + 0);
    constexpr size_t MIN_OVERHEAD = 8;

    on_task_enqueued();
    if constexpr (SizeF + SizeArgs + MIN_OVERHEAD <= SFO_LIMIT) {
      queues.emplace<P>([f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable {
        try {
          f(std::forward<Args>(args)...);
        } catch (...) { // NOLINT(bugprone-empty-catch)
          // Nobody waits for a detached task, so there is no one to report to.
        }
      });
    } else {
      auto ctx = std::make_unique<std::tuple<std::decay_t<F>, std::tuple<std::decay_t<Args>...>>>(
          std::forward<F>(f), std::make_tuple(std::forward<Args>(args)...));
      queues.emplace<P>([ctx = std::move(ctx)]() mutable {
        auto &func = std::get<0>(*ctx);
        auto &base_args = std::get<1>(*ctx);
        try {
          std::apply([&](auto &&...unpacked) { func(std::forward<decltype(unpacked)>(unpacked)...); }, base_args);
        } catch (...) { // NOLINT(bugprone-empty-catch)
          // Nobody waits for a detached task, so there is no one to report to.
        }
      });
    }
    signalWork(1);
  }

  // Case 1: Fast Path WITHOUT Callback (Finished == nullptr_t)
  template <Priority P, typename R, typename F, typename... Args>
  void detachBatchSfoNoCallback(R &&r, F &&f, Args &&...args) {
    for (auto &&item : r) {
      queues.emplace<P>([f, args..., arg = std::move(item)]() mutable {
        try {
          f(std::move(arg), args...);
        } catch (const std::exception &e) {
          ThreadLog::err("Exception: {}", e.what());
        }
      });
    }
  }

  // Case 2: Fast Path WITH Callback
  template <Priority P, typename R, typename F, typename Finished, typename... Args>
  void detachBatchSfoWithCallback(R &&r, F &&f, Finished &&finished, size_t count, Args &&...args) {

    auto state = std::make_shared<DetachBatchState>(count, std::forward<Finished>(finished));

    for (auto &&item : r) {
      queues.emplace<P>([f, args..., arg = std::move(item), state]() mutable {
        auto invoke_finished_if_last = [&]() {
          if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (state->on_finished) {
              state->on_finished();
            }
          }
        };

        try {
          f(std::move(arg), args...);
          invoke_finished_if_last();
        } catch (...) {
          invoke_finished_if_last();
        }
      });
    }
  }

  // =========================================================================
  // HEAP PATH: data is moved to the heap in a MergedState
  // =========================================================================

  // Case 3: Heap Path WITHOUT Callback (Finished == nullptr_t)
  template <Priority P, typename R, typename F, typename... Args>
  void detachBatchHeapNoCallback(R &&r, F &&f, size_t count, Args &&...args) {

    auto shared =
        std::make_shared<DetachedMergedStateNoCallback<R, Args...>>(std::forward<R>(r), std::forward<Args>(args)...);

    for (size_t i = 0; i < count; ++i) {
      queues.emplace<P>([f, shared, i]() {
        try {
          std::apply([&](auto &&...unpacked) { f(std::move(shared->payload[i]), unpacked...); }, shared->saved_args);
        } catch (...) { // NOLINT(bugprone-empty-catch)
          // Nobody waits for a detached task, so there is no one to report to.
        }
      });
    }
  }

  // Case 4: Heap Path WITH Callback
  template <Priority P, typename R, typename F, typename Finished, typename... Args>
  void detachBatchHeapWithCallback(R &&r, F &&f, Finished &&finished, size_t count, Args &&...args) {

    auto shared = std::make_shared<DetachBatchState>(count, std::forward<R>(r), std::forward<Finished>(finished),
                                                     std::forward<Args>(args)...);

    for (size_t i = 0; i < count; ++i) {
      queues.emplace<P>([f, shared, i]() {
        auto invoke_finished_if_last = [&]() {
          if (shared->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (shared->on_finished) {
              shared->on_finished();
            }
          }
        };

        try {
          std::apply([&](auto &&...unpacked) { f(std::move(shared->payload[i]), unpacked...); }, shared->saved_args);
          invoke_finished_if_last();
        } catch (...) {
          invoke_finished_if_last();
        }
      });
    }
  }

  template <Priority P, std::ranges::sized_range R, typename F, typename Finished, typename... Args>
    requires std::invocable<F, std::ranges::range_value_t<R>, Args...> &&
             std::is_void_v<std::invoke_result_t<F, std::ranges::range_value_t<R>, Args...>>
  void detachBatch(R &&r, F &&f, Finished &&finished, Args &&...args) {
    const auto count = std::ranges::size(r);
    if (count == 0) {
      return;
    }

    using ArgType = std::ranges::range_value_t<R>;

    constexpr size_t CAPTURE_BASE = sizeof(std::decay_t<F>) + (0 + ... + sizeof(std::decay_t<Args>)) + sizeof(ArgType);
    constexpr size_t OVERHEAD = sizeof(std::shared_ptr<void>);

    constexpr bool is_null_type = std::is_same_v<std::remove_cvref_t<Finished>, std::nullptr_t>;
    constexpr bool fits_sfo = CAPTURE_BASE + OVERHEAD <= SFO_LIMIT || (!is_null_type && CAPTURE_BASE <= SFO_LIMIT);

    on_task_enqueued(count);
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
    signalWork(count);
  }

} // namespace sc::threading::impl
