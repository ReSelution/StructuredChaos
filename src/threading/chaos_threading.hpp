//
// Created by oleub on 05.03.26.
//

#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <thread>
#include <ranges>
#include <utility>

#include "stats/chaos_counter.hpp"
#include "stats/chaos_stats.hpp"
#include "stats/chaos_throughput.hpp"


namespace SC {
  LOG_ALIAS(ThreadLog, "Chaos", "Threading")

  DEFINE_CHAOS_CORE_STAT(QueueSize, "QueueDepth", SC::ChaosCounter<>, false);

  DEFINE_CHAOS_CORE_STAT(ActiveTask, "ActiveTask", SC::ChaosCounter<>, false);

  DEFINE_CHAOS_CORE_STAT(PoolThroughput, "PoolTasks", SC::ChaosThroughput<SC::MetricUnits>, false);

  DEFINE_CHAOS_CORE_STAT(LongRunningThreads, "LongRunningThreads", SC::ChaosCounter<>, false);

  REGISTER_CHAOS_STAT(QueueSize)
  REGISTER_CHAOS_STAT(ActiveTask)
  REGISTER_CHAOS_STAT(PoolThroughput)
  REGISTER_CHAOS_STAT(LongRunningThreads)

  constexpr uint64_t HELPER_TASK_MULTIPLYER = 4;
  constexpr uint64_t QUEUE_CAP = 50000;

  using MoveOnlyFunction = std::move_only_function<void(int)>;

  namespace Detail {
    struct BatchState {
      std::atomic<size_t> remaining;
      std::promise<void> batch_promise;

      BatchState(size_t n) : remaining(n) {
      }
    };


    template<typename R, typename... Args>
    struct MergedState : BatchState {
      std::decay_t<R> payload;
      std::tuple<std::decay_t<Args>...> saved_args;

      MergedState(size_t n, R &&r, Args &&... a)
        : BatchState(n), payload(std::forward<R>(r)), saved_args(std::forward<Args>(a)...) {
      }
    };

    struct DetachBatchState {
      std::atomic<size_t> remaining;
      MoveOnlyFunction on_finished;

      DetachBatchState(size_t n, MoveOnlyFunction &&cb) : remaining(n), on_finished(std::move(cb)) {
      }
    };

    template<typename R, typename... Args>
    struct DetachedMergedState : DetachBatchState {
      std::decay_t<R> payload;
      std::tuple<std::decay_t<Args>...> saved_args;

      DetachedMergedState(size_t n, R &&r, MoveOnlyFunction &&cb, Args &&... a)
        : DetachBatchState(n, std::move(cb)),
          payload(std::forward<R>(r)),
          saved_args(std::forward<Args>(a)...) {
      }
    };


    template<typename R, typename... Args>
    struct MergedNonVoidState {
      std::decay_t<R> payload;
      std::tuple<std::decay_t<Args>...> saved_args;

      MergedNonVoidState(R &&r, Args &&... a)
        : payload(std::forward<R>(r)), saved_args(std::forward<Args>(a)...) {
      }
    };

    struct BatchContext {
      alignas(64) std::atomic<uint32_t> remaining;

      BatchContext(uint32_t count) : remaining(count) {
      }
    };
  }

  class ChaosThreading {
    static constexpr size_t SFO_LIMIT = 64;

  public:
    enum class Priority : int32_t {
      High,
      Normal,
      Low,
      PriorityCount,
    };

    static void init(uint32_t numThreads = std::thread::hardware_concurrency() - 1);

    static size_t getNumThreads();

    static int32_t getThreadId();

    static void wait_until_finished();

    template<std::ranges::input_range R, typename Func>
    static void parralelFor(R &&range, Func &&func) {
      using IteratorType = std::ranges::iterator_t<R>;
      auto totalSize = std::ranges::distance(range);
      if (totalSize <= 0) [[unlikely]] return;


      const size_t threadLimit = getNumThreads() * HELPER_TASK_MULTIPLYER;
      const size_t numTasksLimit = std::min<size_t>(totalSize, threadLimit);
      const size_t chunkSize = totalSize / numTasksLimit;

      Detail::BatchContext ctx{static_cast<uint32_t>(numTasksLimit)};
      auto rawCtx = &ctx;

      auto current = range.begin();
      auto end = range.end();
      for (uint32_t i = 0; i < numTasksLimit; ++i) {
        auto next = current;
        if (i == numTasksLimit - 1) {
          next = end; // Den Rest einpacken
        } else {
          std::advance(next, chunkSize);
        }
        pushHelperTask([current, next, rawCtx, &func](int id) {
          for (auto it = current; it != next; ++it) {
            auto &&item = *it;
            if constexpr (requires { std::apply(func, item); }) {
              std::apply(func, std::forward<decltype(item)>(item));
            } else {
              func(std::forward<decltype(item)>(item));
            }
          }
          if (rawCtx->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            rawCtx->remaining.notify_one();
          }
        });
        current = next;
      }
      doWork();
      uint32_t val = ctx.remaining.load(std::memory_order_acquire);
      while (val > 0) {
        ctx.remaining.wait(val, std::memory_order_relaxed);
        val = ctx.remaining.load(std::memory_order_acquire);
      }
    };

    template<typename F, typename... Args>
      requires std::invocable<F, int, Args...> && std::is_void_v<std::invoke_result_t<F, int, Args...> >
    void detach(F &&f, Args &&... args) {
      detach<Priority::Normal>(std::forward<F>(f), std::forward<Args>(args)...);
    }

    template<Priority P, typename F, typename... Args>
      requires std::invocable<F, int, Args...> && std::is_void_v<std::invoke_result_t<F, int, Args...> >
    void detach(F &&f, Args &&... args) {
      constexpr size_t SFO_LIMIT = 64;
      constexpr size_t SizeF = sizeof(std::decay_t<F>);
      constexpr size_t SizeArgs = (sizeof(std::decay_t<Args>) + ... + 0);
      constexpr size_t MIN_OVERHEAD = 8; // Für den Promise/Pointer


      if constexpr (SizeF + SizeArgs + MIN_OVERHEAD <= SFO_LIMIT) {
        // CASE 1: Alles passt direkt ins Lambda (SFO-Sieg!)
        pushTask<P>(
          [f = std::forward<F>(f), ...args = std::forward<Args>(args)](int id) mutable {
            try {
              f(id, std::forward<Args>(args)...);
            } catch (...) {
              ThreadLog::err("Exception: {}", std::current_exception());
            }
          });
      } else {
        auto ctx = std::make_unique<std::tuple<std::decay_t<F>, std::tuple<std::decay_t<Args>...> > >(
          std::forward<F>(f),
          std::make_tuple(std::forward<Args>(args)...)
        );

        pushTask<P>([ctx = std::move(ctx)](int id) mutable {
          try {
            auto &func = std::get<0>(*ctx);
            auto &base_args = std::get<1>(*ctx);

            std::apply([&](auto &&... unpacked) {
              func(id, std::forward<decltype(unpacked)>(unpacked)...);
            }, base_args);
          } catch (...) {
            ThreadLog::err("Exception: {}", std::current_exception());
          }
        });
      }
    }


    template<typename F, typename... Args>
      requires std::invocable<F, int, Args...>
    static auto enqueue(F &&f, Args &&... args) -> std::future<std::invoke_result_t<F, int, Args...> > {
      return enqueue<Priority::Normal>(std::forward<F>(f), std::forward<Args>(args)...);
    }

    template<Priority P, typename F, typename... Args>
      requires std::invocable<F, int, Args...>
    static auto enqueue(F &&f, Args &&... args) -> std::future<std::invoke_result_t<F, int, Args...> > {
      using return_type = std::invoke_result_t<F, int, Args...>;

      std::promise<return_type> promise;
      auto res = promise.get_future();

      constexpr size_t SFO_LIMIT = 64;
      constexpr size_t SizeF = sizeof(std::decay_t<F>);
      constexpr size_t SizeArgs = (sizeof(std::decay_t<Args>) + ... + 0);
      constexpr size_t MIN_OVERHEAD = sizeof(std::promise<return_type>);

      if constexpr (SizeF + SizeArgs + MIN_OVERHEAD <= SFO_LIMIT) {
        // CASE 1: Alles passt direkt ins Lambda (SFO-Sieg!)
        pushTask<P>(
          [f = std::forward<F>(f), ...args = std::forward<Args>(args), p = std::move(promise)](int id) mutable {
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
        auto ctx = std::make_unique<std::tuple<std::decay_t<F>, std::tuple<std::decay_t<Args>...> > >(
          std::forward<F>(f),
          std::make_tuple(std::forward<Args>(args)...)
        );

        pushTask<P>([ctx = std::move(ctx), p = std::move(promise)](int id) mutable {
          try {
            auto &func = std::get<0>(*ctx);
            auto &base_args = std::get<1>(*ctx);

            std::apply([&](auto &&... unpacked) {
              if constexpr (std::is_void_v<return_type>) {
                func(id, std::forward<decltype(unpacked)>(unpacked)...);
                p.set_value();
              } else {
                p.set_value(func(id, std::forward<decltype(unpacked)>(unpacked)...));
              }
            }, base_args);
          } catch (...) {
            p.set_exception(std::current_exception());
          }
        });
      }

      return res;
    }

    template<std::ranges::input_range R, typename F, typename Finished, typename... Args>
      requires std::invocable<F, int, std::ranges::range_value_t<R>, Args...> && std::is_void_v<std::invoke_result_t<F,
                 int, std::ranges::range_value_t<R>, Args...> >
    static void detacheBatch(R &&r, F &&f, Finished &&finished, Args &&... args) {
      detacheBatch<Priority::Normal>(std::forward<R>(r), std::forward<F>(f), std::forward<Finished>(finished),
                                     std::forward<Args>(args)...);
    }

    template<Priority P, std::ranges::input_range R, typename F, typename Finished, typename... Args>
      requires std::invocable<F, int, std::ranges::range_value_t<R>, Args...> && std::is_void_v<std::invoke_result_t<F,
                 int, std::ranges::range_value_t<R>, Args...> >
    static void detacheBatch(R &&r, F &&f, Finished &&finished, Args &&... args) {
      const auto count = r.size();
      if (count == 0) { return; }
      using ArgType = std::ranges::range_value_t<R>;

      constexpr size_t SFO_LIMIT = 64;
      constexpr size_t CAPTURE_BASE = sizeof(std::decay_t<F>) + (0 + ... + sizeof(std::decay_t<Args>)) + sizeof(
                                        ArgType);
      constexpr size_t OVERHEAD = sizeof(std::shared_ptr<void>);

      std::vector<MoveOnlyFunction> batch;
      batch.reserve(count);
      auto arg_tuple = std::make_tuple(std::forward<Args>(args)...);
      constexpr bool is_null_type = std::is_same_v<std::remove_cvref_t<Finished>, std::nullptr_t>;

      if constexpr (CAPTURE_BASE + OVERHEAD <= SFO_LIMIT || (!is_null_type && CAPTURE_BASE <= SFO_LIMIT)) {
        if constexpr (is_null_type) {
          for (auto &&item: r) {
            batch.emplace_back([f, args..., arg = std::move(item)](int id) mutable {
              try {
                f(id, std::move(arg), args...);
              } catch (...) {
              }
            });
          }
        } else {
          auto state = std::make_shared<Detail::DetachBatchState>(count, std::forward<Finished>(finished));
          for (auto &&item: r) {
            batch.emplace_back([f, args..., arg = std::move(item), state](int id) mutable {
              try {
                f(id, std::move(arg), args...);
                if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                  if (state->on_finished) state->on_finished(id);
                }
              } catch (...) {
                if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                  if (state->on_finished) state->on_finished(id);
                }
              }
            });
          }
        }
        pushBatch<P>(std::move(batch));
      } else {
        if constexpr (is_null_type) {
          // CASE: Kein Callback -> Nutze Standard MergedState
          auto shared = std::make_shared<Detail::MergedState<R, Args...> >(
            count, std::forward<R>(r), std::forward<Args>(args)...);

          for (size_t i = 0; i < count; ++i) {
            batch.emplace_back([f, shared, i](int id) {
              try {
                std::apply([&](auto &&... unpacked) {
                  f(id, std::move(shared->payload[i]), unpacked...);
                }, shared->saved_args);

                if (shared->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                  shared->batch_promise.set_value();
                }
              } catch (...) {
                shared->batch_promise.set_exception(std::current_exception());
              }
            });
          }
        } else {
          // CASE: Mit Callback -> Nutze spezialisierte Struktur
          struct DetachedMergedState : Detail::MergedState<R, Args...> {
            std::decay_t<Finished> on_finished;

            DetachedMergedState(size_t n, R &&r, Finished &&cb, Args &&... a)
              : Detail::MergedState<R, Args...>(n, std::forward<R>(r), std::forward<Args>(a)...),
                on_finished(std::forward<Finished>(cb)) {
            }
          };

          auto shared = std::make_shared<DetachedMergedState>(
            count, std::forward<R>(r), std::forward<Finished>(finished), std::forward<Args>(args)...);

          for (size_t i = 0; i < count; ++i) {
            batch.emplace_back([f, shared, i](int id) {
              try {
                std::apply([&](auto &&... unpacked) {
                  f(id, std::move(shared->payload[i]), unpacked...);
                }, shared->saved_args);

                if (shared->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                  if (shared->on_finished) shared->on_finished(id);
                  shared->batch_promise.set_value();
                }
              } catch (...) {
                shared->batch_promise.set_exception(std::current_exception());
                if (shared->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                  if (shared->on_finished) shared->on_finished(id);
                }
              }
            });
          }
        }
        pushBatch<P>(std::move(batch));
      }
    }


    template<std::ranges::input_range R, typename F, typename... Args>
      requires std::invocable<F, int, std::ranges::range_value_t<R>, Args...>
    static auto enqueueBatch(R &&r, F &&f, Args &&... args) {
      return enqueueBatch<Priority::Normal>(std::forward<R>(r), std::forward<F>(f), std::forward<Args>(args)...);
    }

    template<Priority P, std::ranges::input_range R, typename F, typename... Args>
      requires std::invocable<F, int, std::ranges::range_value_t<R>, Args...>
    static auto enqueueBatch(R &&r, F &&f, Args &&... args) {
      const size_t count = r.size();
      using ArgType = std::ranges::range_value_t<R>;
      using RetType = std::invoke_result_t<F, int, ArgType, Args...>;
      constexpr bool is_void = std::is_void_v<RetType>;

      constexpr size_t SFO_LIMIT = 64;
      constexpr size_t CAPTURE_BASE = sizeof(std::decay_t<F>) + (0 + ... + sizeof(std::decay_t<Args>)) + sizeof(
                                        ArgType);
      constexpr size_t OVERHEAD = is_void ? sizeof(std::shared_ptr<void>) : sizeof(std::promise<RetType>);

      std::vector<MoveOnlyFunction> batch;
      batch.reserve(count);
      auto arg_tuple = std::make_tuple(std::forward<Args>(args)...);

      // CASE 1: SFO (Fast-Path)
      if constexpr (CAPTURE_BASE + OVERHEAD <= SFO_LIMIT) {
        if constexpr (is_void) {
          auto state = std::make_shared<Detail::BatchState>(count);
          for (auto &&item: r) {
            using ForwardType = std::conditional_t<std::is_lvalue_reference_v<R>,
              decltype(item) &,
              std::remove_reference_t<decltype(item)> &&>;
            batch.emplace_back(
              make_void_individual(f, static_cast<ForwardType>(item), arg_tuple, state));
          }
          pushBatch<P>(std::move(batch));
          return state->batch_promise.get_future();
        } else {
          std::vector<std::future<RetType> > futures;
          futures.reserve(count);
          for (auto &&item: r) {
            std::promise<RetType> p;
            futures.push_back(p.get_future());
            batch.emplace_back(make_nonvoid_individual(f, std::forward<decltype(item)>(item), arg_tuple, std::move(p)));
          }
          pushBatch<P>(std::move(batch));
          return futures;
        }
      }
      // CASE 2: MERGED (Memory Efficient)
      else {
        if constexpr (is_void) {
          auto shared = std::make_shared<Detail::MergedState<R, Args...> >(
            count, std::forward<R>(r), std::forward<Args>(args)...);
          for (size_t i = 0; i < count; ++i) batch.emplace_back(make_void_merged(f, shared, i));
          pushBatch<P>(std::move(batch));
          return shared->batch_promise.get_future();
        } else {
          auto shared = std::make_shared<Detail::MergedNonVoidState<R, Args...> >(
            std::forward<R>(r), std::forward<Args>(args)...);
          std::vector<std::future<RetType> > futures;
          futures.reserve(count);
          for (size_t i = 0; i < count; ++i) {
            std::promise<RetType> p;
            futures.push_back(p.get_future());
            batch.emplace_back([f, shared, i, p = std::move(p)](int id) mutable {
              try {
                std::apply([&](auto &&... ex) { p.set_value(f(id, std::move(shared->payload[i]), ex...)); },
                           shared->saved_args);
              } catch (...) { p.set_exception(std::current_exception()); }
            });
          }
          pushBatch<P>(std::move(batch));
          return futures;
        }
      }
    }


    static MoveOnlyFunction helpThread(int id);

    template<typename F, typename... Args>
    static auto
    enqueueLong(std::string_view name, F &&f, Args &&... args) -> std::future<std::invoke_result_t<F, Args...> > {
      using return_type = std::invoke_result_t<F, std::stop_token, Args...>;

      auto promise = std::promise<return_type>();
      auto res = promise->get_future();

      auto boundTask = [promise = std::move(promise), f = std::forward<F>(f), ...args = std::forward<Args>(args)
          ](std::stop_token st) mutable {
        if constexpr (std::is_void_v<return_type>) {
          f(st, std::move(args)...);
          promise->set_value();
        } else {
          promise.set_value(f(st, std::move(args)...));
        }
      };
      pushLongTaskInternal(name, std::move(boundTask));
      return res;
    }

    template<class F, class... Args>
    static auto enqueueLong(F &&f, Args &&... args) -> std::future<std::invoke_result_t<F, Args...> > {
      return enqueueLong("ChaosLongTask", std::forward<F>(f), std::forward<Args>(args)...);
    }

  private:
    template<Priority P>
    static void pushTask(MoveOnlyFunction &&task);

    template<Priority P>
    static void pushBatch(std::vector<MoveOnlyFunction> &&batch);

    static void pushLongTaskInternal(std::string_view name, std::function<void(std::stop_token)> task);

    static void pushHelperTask(MoveOnlyFunction &&task);

    static void doWork();

    // --- VOID PATHS ---
    template<typename F, typename Arg, typename... Args>
    static auto make_void_individual(F &&f, Arg &&item, const std::tuple<Args...> &args,
                                     std::shared_ptr<Detail::BatchState> &state) {
      return [f, args, item = std::forward<Arg>(item), state](int id) mutable {
        try {
          std::apply([&](auto &&... extra) { f(id, std::move(item), extra...); }, args);
          if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) state->batch_promise.set_value();
        } catch (...) { state->batch_promise.set_exception(std::current_exception()); }
      };
    }

    template<typename F, typename Arg, typename... Args>
    static auto make_void_individualWithCallback(F &&f, Arg &&item, const std::tuple<Args...> &args,
                                                 std::shared_ptr<Detail::DetachBatchState> &state) {
      return [f, args , item = std::forward<Arg>(item), state](int id) mutable {
        try {
          std::apply([&](auto &&... extra) { f(id, std::move(item), extra...); }, args);

          if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (state->on_finished) state->on_finished(id);
          }
        } catch (...) {
          if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (state->on_finished) state->on_finished(id);
          }
        }
      };
    }

    template<typename F, typename Shared>
    static auto make_void_merged(F &&f, Shared shared, size_t i) {
      return [f, shared, i](int id) {
        try {
          std::apply([&](auto &&... extra) { f(id, std::move(shared->payload[i]), extra...); }, shared->saved_args);
          if (shared->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) shared->batch_promise.set_value();
        } catch (...) { shared->batch_promise.set_exception(std::current_exception()); }
      };
    }

    // --- NON-VOID PATHS ---
    template<typename F, typename Arg, typename... Args, typename Promise>
    static auto make_nonvoid_individual(F &&f, Arg &&item, const std::tuple<Args...> &args, Promise p) {
      return [f, args, item = std::forward<Arg>(item), p = std::move(p)](int id) mutable {
        try {
          std::apply([&](auto &&... extra) { p.set_value(f(id, std::move(item), extra...)); }, args);
        } catch (...) { p.set_exception(std::current_exception()); }
      };
    }
  };
} // SC
