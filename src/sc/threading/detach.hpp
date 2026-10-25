#pragma once

#include <concepts>
#include <cstddef>
#include <memory>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>

#include "backend.hpp"
#include "internal.hpp"
#include "types.hpp"

namespace sc::threading::internal {

  template<typename F, typename... Args>
    requires std::invocable<F, Args...> && std::is_void_v<std::invoke_result_t<F, Args...>>
  void detach(F &&f, Args &&...args) {
    constexpr size_t CAPTURE_SIZE = sizeof(std::decay_t<F>) + (sizeof(std::decay_t<Args>) + ... + 0);

    if constexpr (CAPTURE_SIZE <= INLINE_CAPTURE_LIMIT) {
      queue.submit([f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable {
        try {
          f(std::forward<Args>(args)...);
        } catch (...) {
          logDetachedException();
        }
      });
    } else {
      auto ctx = std::make_unique<std::tuple<std::decay_t<F>, std::tuple<std::decay_t<Args>...>>>(
          std::forward<F>(f), std::make_tuple(std::forward<Args>(args)...));
      queue.submit([ctx = std::move(ctx)]() mutable {
        auto &func = std::get<0>(*ctx);
        auto &savedArgs = std::get<1>(*ctx);
        try {
          std::apply([&](auto &&...unpacked) { func(std::forward<decltype(unpacked)>(unpacked)...); }, savedArgs);
        } catch (...) {
          logDetachedException();
        }
      });
    }
    signalWork(1);
  }

  // The batch paths: the function, the extra arguments and one element are
  // either copied into every task (inline) or kept once in a shared state that
  // all tasks of the batch index into (merged). Elements are moved out of the
  // range only if it was passed as an rvalue.

  template<typename R, typename F, typename... Args>
  void detachBatchInline(R &&r, F &&f, Args &&...args) {
    for (auto &&item: r) {
      queue.submit([f, args..., arg = forwardElement<R>(item)]() mutable {
        try {
          f(std::move(arg), args...);
        } catch (...) {
          logDetachedException();
        }
      });
    }
  }

  template<typename R, typename F, typename Finished, typename... Args>
  void detachBatchInlineWithCallback(R &&r, F &&f, Finished &&finished, size_t count, Args &&...args) {
    auto state = std::make_shared<DetachBatchState>(count, std::forward<Finished>(finished));

    for (auto &&item: r) {
      queue.submit([f, args..., arg = forwardElement<R>(item), state]() mutable {
        try {
          f(std::move(arg), args...);
        } catch (...) {
          logDetachedException();
        }
        state->finishTask();
      });
    }
  }

  template<typename R, typename F, typename... Args>
  void detachBatchMerged(R &&r, F &&f, size_t count, Args &&...args) {
    auto shared =
        std::make_shared<DetachedMergedStateNoCallback<R, Args...>>(std::forward<R>(r), std::forward<Args>(args)...);

    for (size_t i = 0; i < count; ++i) {
      queue.submit([f, shared, i]() mutable {
        try {
          std::apply([&](auto &&...unpacked) { f(std::move(shared->payload[i]), unpacked...); }, shared->savedArgs);
        } catch (...) {
          logDetachedException();
        }
      });
    }
  }

  template<typename R, typename F, typename Finished, typename... Args>
  void detachBatchMergedWithCallback(R &&r, F &&f, Finished &&finished, size_t count, Args &&...args) {
    auto shared = std::make_shared<DetachedMergedState<R, Args...>>(
        count, std::forward<R>(r), MoveOnlyFunction(std::forward<Finished>(finished)), std::forward<Args>(args)...);

    for (size_t i = 0; i < count; ++i) {
      queue.submit([f, shared, i]() mutable {
        try {
          std::apply([&](auto &&...unpacked) { f(std::move(shared->payload[i]), unpacked...); }, shared->savedArgs);
        } catch (...) {
          logDetachedException();
        }
        shared->finishTask();
      });
    }
  }

  template<std::ranges::sized_range R, typename F, typename Finished, typename... Args>
    requires std::invocable<F, std::ranges::range_value_t<R>, Args...> &&
             std::is_void_v<std::invoke_result_t<F, std::ranges::range_value_t<R>, Args...>>
  void detachBatch(R &&r, F &&f, Finished &&finished, Args &&...args) {
    const auto count = static_cast<size_t>(std::ranges::size(r));
    if (count == 0) {
      return;
    }

    using ArgType = std::ranges::range_value_t<R>;

    constexpr size_t CAPTURE_SIZE = sizeof(std::decay_t<F>) + (0 + ... + sizeof(std::decay_t<Args>)) + sizeof(ArgType);
    constexpr bool hasCallback = !std::is_same_v<std::remove_cvref_t<Finished>, std::nullptr_t>;
    constexpr size_t STATE_SIZE = hasCallback ? sizeof(std::shared_ptr<void>) : 0;
    // The merged state keeps a copy of the range and indexes into it.
    constexpr bool inlineCaptures =
        CAPTURE_SIZE + STATE_SIZE <= INLINE_CAPTURE_LIMIT || !std::ranges::random_access_range<std::decay_t<R>>;

    if constexpr (inlineCaptures) {
      if constexpr (hasCallback) {
        detachBatchInlineWithCallback(std::forward<R>(r), std::forward<F>(f), std::forward<Finished>(finished), count,
                                      std::forward<Args>(args)...);
      } else {
        detachBatchInline(std::forward<R>(r), std::forward<F>(f), std::forward<Args>(args)...);
      }
    } else {
      if constexpr (hasCallback) {
        detachBatchMergedWithCallback(std::forward<R>(r), std::forward<F>(f), std::forward<Finished>(finished), count,
                                      std::forward<Args>(args)...);
      } else {
        detachBatchMerged(std::forward<R>(r), std::forward<F>(f), count, std::forward<Args>(args)...);
      }
    }
    signalWork(count);
  }

} // namespace sc::threading::internal
