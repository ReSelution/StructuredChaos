module;

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <ranges>
#include <vector>
export module sc.threading:enqueue;

import :types;
import :internal;
import :backend;

namespace sc::threading::impl {


  template<Priority P, typename F, typename... Args>
    requires std::invocable<F, int, Args...>
  auto enqueue(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, int, Args...>> {
    using return_type = std::invoke_result_t<F, int, Args...>;
    std::promise<return_type> promise;
    auto res = promise.get_future();

    constexpr size_t SizeF        = sizeof(std::decay_t<F>);
    constexpr size_t SizeArgs     = (sizeof(std::decay_t<Args>) + ... + 0);
    constexpr size_t MIN_OVERHEAD = sizeof(std::promise<return_type>);

    if constexpr (SizeF + SizeArgs + MIN_OVERHEAD <= SFO_LIMIT) {
      pushTask<P>(
          [f = std::forward<F>(f), ... args = std::forward<Args>(args), p = std::move(promise)](int id) mutable {
            try {

              if constexpr (std::is_void_v<return_type>) {
                f(id, std::forward<Args>(args)...);
                p.set_value();
              } else {
                p.set_value(f(id, std::forward<Args>(args)...));
              }
            } catch (...) {
              p.set_exception(std::current_exception());
            }
          });
    } else {
      auto ctx = std::make_unique<std::tuple<std::decay_t<F>, std::tuple<std::decay_t<Args>...>>>(
          std::forward<F>(f), std::make_tuple(std::forward<Args>(args)...));
      pushTask<P>([ctx = std::move(ctx), p = std::move(promise)](int id) mutable {
        try {
          auto &func      = std::get<0>(*ctx);
          auto &base_args = std::get<1>(*ctx);
          std::apply(
              [&](auto &&...unpacked) {
                if constexpr (std::is_void_v<return_type>) {
                  func(id, std::forward<decltype(unpacked)>(unpacked)...);
                  p.set_value();
                } else {
                  p.set_value(func(id, std::forward<decltype(unpacked)>(unpacked)...));
                }
              },
              base_args);
        } catch (...) {
          p.set_exception(std::current_exception());
        }
      });
    }
    return res;
  }

  // --- VOID PATHS ---
  template<typename F, typename Arg, typename... Args>
  auto make_void_individual(F &&f, Arg &&item, const std::tuple<Args...> &args, std::shared_ptr<BatchState> &state) {
    return [f, args, item = std::forward<Arg>(item), state](int id) mutable {
      auto completion_guard = [&]() {
        if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          // Der LETZTE Thread setzt den Erfolg, WENN keine Exception geworfen wurde
          if (!state->exception_set.test_and_set(std::memory_order_relaxed)) {
            state->batch_promise.set_value();
          }
        }
      };
      try {
        std::apply([&](auto &&...extra) { f(id, std::move(item), extra...); }, args);
        completion_guard();
      } catch (...) {
        if (!state->exception_set.test_and_set(std::memory_order_relaxed)) {
          state->batch_promise.set_exception(std::current_exception());
        }
        completion_guard();
      }
    };
  }

  template<typename F, typename Arg, typename... Args>
  auto make_void_individualWithCallback(F &&f, Arg &&item, const std::tuple<Args...> &args,
                                        std::shared_ptr<DetachBatchState> &state) {
    return [f, args, item = std::forward<Arg>(item), state](int id) mutable {
      try {
        std::apply([&](auto &&...extra) { f(id, std::move(item), extra...); }, args);

        if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          if (state->on_finished)
            state->on_finished(id);
        }
      } catch (...) {
        if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          if (state->on_finished)
            state->on_finished(id);
        }
      }
    };
  }

  template<typename F, typename Shared>
  auto make_void_merged(F &&f, Shared shared, size_t i) {
    return [f, shared, i](int id) {
      try {
        std::apply([&](auto &&...extra) { f(id, std::move(shared->payload[i]), extra...); }, shared->saved_args);
        if (shared->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
          shared->batch_promise.set_value();
      } catch (...) {
        shared->batch_promise.set_exception(std::current_exception());
      }
    };
  }

  // --- NON-VOID PATHS ---
  template<typename F, typename Arg, typename... Args, typename Promise>
  auto make_nonvoid_individual(F &&f, Arg &&item, const std::tuple<Args...> &args, Promise p) {
    return [f, args, item = std::forward<Arg>(item), p = std::move(p)](int id) mutable {
      try {
        std::apply([&](auto &&...extra) { p.set_value(f(id, std::move(item), extra...)); }, args);
      } catch (...) {
        p.set_exception(std::current_exception());
      }
    };
  }

  // =========================================================================
  // CASE 1: SFO (Fast-Path)
  // =========================================================================

  // Case 1a: SFO + Void Return Type -> Liefert std::future<void>
  template<Priority P, typename R, typename F, typename TupleArgs>
  auto enqueueBatchSfoVoid(R &&r, F &&f, TupleArgs &&arg_tuple) {
    const size_t size = std::ranges::size(r);


    auto state = std::make_shared<BatchState>(size);

    for (auto &&item: r) {
      using ForwardType = std::conditional_t<std::is_lvalue_reference_v<R>, decltype(item) &,
                                             std::remove_reference_t<decltype(item)> &&>;

      queues.emplace<P>(make_void_individual(f, static_cast<ForwardType>(item), arg_tuple, state));
    }

    return state->batch_promise.get_future();
  }

  // Case 1b: SFO + Non-Void Return Type -> Liefert std::vector<std::future<RetType>>
  template<Priority P, typename RetType, typename R, typename F, typename TupleArgs>
  auto enqueueBatchSfoNonVoid(R &&r, size_t size, F &&f, TupleArgs &&arg_tuple) {

    FutureGroup<RetType> futures;
    futures.reserve(size);

    for (auto &&item: r) {
      std::promise<RetType> p;
      futures.push_back(p.get_future());
      queues.emplace<P>(make_nonvoid_individual(f, std::forward<decltype(item)>(item), arg_tuple, std::move(p)));
    }

    return futures;
  }

  // =========================================================================
  // CASE 2: MERGED (Memory-Efficient / Heap-Path)
  // =========================================================================

  // Case 2a: Merged + Void Return Type -> Liefert std::future<void>
  template<Priority P, typename R, typename F, typename... Args>
  auto enqueueBatchMergedVoid(R &&r, F &&f, Args &&...args) {
    const size_t size = std::ranges::size(r);
    auto shared = std::make_shared<MergedState<R, Args...>>(size, std::forward<R>(r), std::forward<Args>(args)...);

    for (size_t i = 0; i < size; ++i) {
      queues.emplace<P>([f, shared, i](int id) {
        auto completion_guard = [&]() {
          if (shared->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (!shared->exception_set.test_and_set(std::memory_order_relaxed)) {
              shared->batch_promise.set_value();
            }
          }
        };
        try {
          std::apply([&](auto &&...unpacked) { f(id, std::move(shared->payload[i]), unpacked...); },
                     shared->saved_args);
          completion_guard();
        } catch (...) {
          if (!shared->exception_set.test_and_set(std::memory_order_relaxed)) {
            shared->batch_promise.set_exception(std::current_exception());
          }
          completion_guard();
        }
      });
    }

    return shared->batch_promise.get_future();
  }

  // Case 2b: Merged + Non-Void Return Type -> Liefert std::vector<std::future<RetType>>
  template<Priority P, typename RetType, typename R, typename F, typename... Args>
  auto enqueueBatchMergedNonVoid(R &&r, F &&f, Args &&...args) {
    struct MergedNonVoidState : MergedState<R, Args...> {
      std::vector<std::promise<RetType>> promises;

      MergedNonVoidState(size_t n, R &&r, Args &&...a) :
          MergedState<R, Args...>(n, std::forward<R>(r), std::forward<Args>(a)...), promises(n) {}
    };
    const size_t size = std::ranges::size(r);


    auto shared = std::make_shared<MergedNonVoidState>(size, std::forward<R>(r), std::forward<Args>(args)...);
    FutureGroup<RetType> futures;
    futures.reserve(size);
    for (auto &p: shared->promises) {
      futures.push_back(p.get_future());
    }

    for (size_t i = 0; i < size; ++i) {
      queues.emplace<P>([f, shared, i](int id) {
        try {
          if constexpr (std::is_move_constructible_v<RetType>) {
            RetType res =
                std::apply([&](auto &&...unpacked) { return f(id, std::move(shared->payload[i]), unpacked...); },
                           shared->saved_args);
            shared->promises[i].set_value(std::move(res));
          } else {
            shared->promises[i].set_value(
                std::apply([&](auto &&...unpacked) { return f(id, std::move(shared->payload[i]), unpacked...); },
                           shared->saved_args));
          }
        } catch (...) {
          shared->promises[i].set_exception(std::current_exception());
        }
      });
    }

    return futures;
  }

  export template<Priority P, std::ranges::sized_range R, typename F, typename... Args>
    requires std::invocable<F, int, std::ranges::range_value_t<R>, Args...>
  auto enqueueBatch(R &&r, F &&f, Args &&...args) {
    const size_t count     = std::ranges::size(r);
    using ArgType          = std::ranges::range_value_t<R>;
    using RetType          = std::invoke_result_t<F, int, ArgType, Args...>;
    constexpr bool is_void = std::is_void_v<RetType>;

    constexpr size_t CAPTURE_BASE = sizeof(std::decay_t<F>) + (0 + ... + sizeof(std::decay_t<Args>)) + sizeof(ArgType);
    constexpr size_t OVERHEAD     = is_void ? sizeof(std::shared_ptr<void>) : sizeof(std::promise<RetType>);

    constexpr bool fits_sfo = CAPTURE_BASE + OVERHEAD <= SFO_LIMIT;


    auto dispatch = [&]() {
      if constexpr (fits_sfo) {
        auto arg_tuple = std::make_tuple(std::forward<Args>(args)...);

        if constexpr (is_void) {
          return enqueueBatchSfoVoid<P>(std::forward<R>(r), std::forward<F>(f), std::move(arg_tuple));
        } else {
          return enqueueBatchSfoNonVoid<P, RetType>(std::forward<R>(r), count, std::forward<F>(f),
                                                    std::move(arg_tuple));
        }
      } else {
        if constexpr (is_void) {
          return enqueueBatchMergedVoid<P>(std::forward<R>(r), std::forward<F>(f), std::forward<Args>(args)...);
        } else {
          return enqueueBatchMergedNonVoid<P, RetType>(std::forward<R>(r), std::forward<F>(f),
                                                       std::forward<Args>(args)...);
        }
      }
    };

    auto futures = dispatch();
    QueueSize::record(count);
    signalWork(count);
    return futures;
  }
} // namespace sc::threading::impl
