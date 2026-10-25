#pragma once

#include <concepts>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "backend.hpp"
#include "future_group.hpp"
#include "internal.hpp"
#include "types.hpp"

namespace sc::threading::internal {

  template<typename F, typename... Args>
    requires std::invocable<F, Args...>
  auto enqueue(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
    using ReturnType = std::invoke_result_t<F, Args...>;
    std::promise<ReturnType> promise;
    auto result = promise.get_future();

    constexpr size_t CAPTURE_SIZE =
        sizeof(std::decay_t<F>) + (sizeof(std::decay_t<Args>) + ... + 0) + sizeof(std::promise<ReturnType>);

    if constexpr (CAPTURE_SIZE <= INLINE_CAPTURE_LIMIT) {
      queue.submit([f = std::forward<F>(f), ... args = std::forward<Args>(args), p = std::move(promise)]() mutable {
        try {
          if constexpr (std::is_void_v<ReturnType>) {
            f(std::forward<Args>(args)...);
            p.set_value();
          } else {
            p.set_value(f(std::forward<Args>(args)...));
          }
        } catch (...) {
          p.set_exception(std::current_exception());
        }
      });
    } else {
      auto ctx = std::make_unique<std::tuple<std::decay_t<F>, std::tuple<std::decay_t<Args>...>>>(
          std::forward<F>(f), std::make_tuple(std::forward<Args>(args)...));
      queue.submit([ctx = std::move(ctx), p = std::move(promise)]() mutable {
        try {
          auto &func = std::get<0>(*ctx);
          auto &savedArgs = std::get<1>(*ctx);
          std::apply(
              [&](auto &&...unpacked) {
                if constexpr (std::is_void_v<ReturnType>) {
                  func(std::forward<decltype(unpacked)>(unpacked)...);
                  p.set_value();
                } else {
                  p.set_value(func(std::forward<decltype(unpacked)>(unpacked)...));
                }
              },
              savedArgs);
        } catch (...) {
          p.set_exception(std::current_exception());
        }
      });
    }
    signalWork(1);
    return result;
  }

  // The batch paths: the function, the extra arguments and one element are
  // either copied into every task (inline) or kept once in a shared state that
  // all tasks of the batch index into (merged). Elements are moved out of the
  // range only if it was passed as an rvalue.

  // Without results: one future for the whole batch, see BatchState.
  template<typename R, typename F, typename... Args>
  std::future<void> enqueueBatchInlineVoid(R &&r, size_t size, F &&f, Args &&...args) {
    auto state = std::make_shared<BatchState>(size);
    auto future = state->batchPromise.get_future();
    if (size == 0) {
      // No task would ever complete it.
      state->batchPromise.set_value();
    }

    for (auto &&item: r) {
      queue.submit([f, args..., arg = forwardElement<R>(item), state]() mutable {
        std::exception_ptr error;
        try {
          f(std::move(arg), args...);
        } catch (...) {
          error = std::current_exception();
        }
        state->finishTask(std::move(error));
      });
    }
    return future;
  }

  // With results: one future per element.
  template<typename RetType, typename R, typename F, typename... Args>
  FutureGroup<RetType> enqueueBatchInline(R &&r, size_t size, F &&f, Args &&...args) {
    FutureGroup<RetType> futures;
    futures.reserve(size);

    for (auto &&item: r) {
      std::promise<RetType> promise;
      futures.push(promise.get_future());
      queue.submit([f, args..., arg = forwardElement<R>(item), p = std::move(promise)]() mutable {
        try {
          p.set_value(f(std::move(arg), args...));
        } catch (...) {
          p.set_exception(std::current_exception());
        }
      });
    }
    return futures;
  }

  template<typename R, typename F, typename... Args>
  std::future<void> enqueueBatchMergedVoid(R &&r, size_t size, F &&f, Args &&...args) {
    auto shared = std::make_shared<MergedState<R, Args...>>(size, std::forward<R>(r), std::forward<Args>(args)...);
    auto future = shared->batchPromise.get_future();
    if (size == 0) {
      shared->batchPromise.set_value();
    }

    for (size_t i = 0; i < size; ++i) {
      queue.submit([f, shared, i]() mutable {
        std::exception_ptr error;
        try {
          std::apply([&](auto &&...unpacked) { f(std::move(shared->payload[i]), unpacked...); }, shared->savedArgs);
        } catch (...) {
          error = std::current_exception();
        }
        shared->finishTask(std::move(error));
      });
    }
    return future;
  }

  template<typename RetType, typename R, typename F, typename... Args>
  FutureGroup<RetType> enqueueBatchMerged(R &&r, size_t size, F &&f, Args &&...args) {
    struct State : MergedState<R, Args...> {
      std::vector<std::promise<RetType>> promises;

      State(size_t n, R &&r, Args &&...a) :
          MergedState<R, Args...>(n, std::forward<R>(r), std::forward<Args>(a)...), promises(n) {}
    };

    auto shared = std::make_shared<State>(size, std::forward<R>(r), std::forward<Args>(args)...);
    FutureGroup<RetType> futures;
    futures.reserve(size);
    for (auto &p: shared->promises) {
      futures.push(p.get_future());
    }

    for (size_t i = 0; i < size; ++i) {
      queue.submit([f, shared, i]() mutable {
        try {
          shared->promises[i].set_value(std::apply(
              [&](auto &&...unpacked) { return f(std::move(shared->payload[i]), unpacked...); }, shared->savedArgs));
        } catch (...) {
          shared->promises[i].set_exception(std::current_exception());
        }
      });
    }
    return futures;
  }

  template<std::ranges::sized_range R, typename F, typename... Args>
    requires std::invocable<F, std::ranges::range_value_t<R>, Args...>
  auto enqueueBatch(R &&r, F &&f, Args &&...args) {
    const auto count = static_cast<size_t>(std::ranges::size(r));
    using ArgType = std::ranges::range_value_t<R>;
    using RetType = std::invoke_result_t<F, ArgType, Args...>;
    constexpr bool isVoid = std::is_void_v<RetType>;

    constexpr size_t CAPTURE_SIZE = sizeof(std::decay_t<F>) + (0 + ... + sizeof(std::decay_t<Args>)) + sizeof(ArgType);
    constexpr size_t STATE_SIZE = isVoid ? sizeof(std::shared_ptr<void>) : sizeof(std::promise<RetType>);
    // The merged state keeps a copy of the range and indexes into it.
    constexpr bool inlineCaptures =
        CAPTURE_SIZE + STATE_SIZE <= INLINE_CAPTURE_LIMIT || !std::ranges::random_access_range<std::decay_t<R>>;

    auto dispatch = [&]() {
      if constexpr (inlineCaptures) {
        if constexpr (isVoid) {
          return enqueueBatchInlineVoid(std::forward<R>(r), count, std::forward<F>(f), std::forward<Args>(args)...);
        } else {
          return enqueueBatchInline<RetType>(std::forward<R>(r), count, std::forward<F>(f),
                                             std::forward<Args>(args)...);
        }
      } else {
        if constexpr (isVoid) {
          return enqueueBatchMergedVoid(std::forward<R>(r), count, std::forward<F>(f), std::forward<Args>(args)...);
        } else {
          return enqueueBatchMerged<RetType>(std::forward<R>(r), count, std::forward<F>(f),
                                             std::forward<Args>(args)...);
        }
      }
    };

    auto futures = dispatch();
    signalWork(count);
    return futures;
  }
} // namespace sc::threading::internal
