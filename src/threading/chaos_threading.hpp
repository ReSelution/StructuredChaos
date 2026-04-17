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

  class ChaosThreading {
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
      auto totalSize = std::ranges::distance(range);
      if (totalSize <= 0) [[unlikely]] return;


      const size_t threadLimit = getNumThreads() * HELPER_TASK_MULTIPLYER;
      const size_t numTasksLimit = std::min<size_t>(totalSize, threadLimit);

      const size_t chunkSize = totalSize / numTasksLimit;


      struct alignas(64) BatchContext {
        std::atomic<uint32_t> remaining;
        std::promise<void> promise;

        BatchContext(uint32_t count) : remaining(count) {}
      };

      BatchContext ctx(static_cast<uint32_t>(numTasksLimit));
      auto ftr = ctx.promise.get_future();
      auto rawCtx = &ctx;

      auto current = range.begin();
      auto end = range.end();
      for (uint32_t i = 0; i < numTasksLimit; ++i) {
        auto next = current;
        if (i == numTasksLimit - 1) {
          next = end; // Den Rest einpacken
        }
        else {
          std::advance(next, chunkSize);
        }
        pushHelperTask([current, next, rawCtx, &func](int id) {
          for (auto it = current; it != next; ++it) {
            auto &&item = *it;
            if constexpr (requires { std::apply(func, item); }) {
              std::apply(func, std::forward<decltype(item)>(item));
            }
            else {
              func(std::forward<decltype(item)>(item));
            }
          }
          if (rawCtx->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            rawCtx->promise.set_value();
          }
        });
        current = next;
      }
      doWork();
      ftr.wait();

    };

    template<typename F, typename... Args>
    requires std::invocable<F, int, Args...> && std::is_void_v<std::invoke_result_t<F, int, Args...>>
    void detach(F &&f, Args &&...args) {
      detach<Priority::Normal>(std::forward<F>(f), std::forward<Args>(args) ...);
    }

    template<Priority P, typename F, typename... Args>
    requires std::invocable<F, int, Args...> && std::is_void_v<std::invoke_result_t<F, int, Args...>>
    void detach(F &&f, Args &&...args) {


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
      }
      else {
        auto ctx = std::make_unique<std::tuple<std::decay_t<F>, std::tuple<std::decay_t<Args>...>>>(
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
      return enqueue<Priority::Normal>(std::forward<F>(f), std::forward<Args>(args) ...);
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
                }
                else {
                  p.set_value(f(id, std::forward<Args>(args)...));
                }
              } catch (...) {
                p.set_exception(std::current_exception());
              }
            });
      }
      else {
        auto ctx = std::make_unique<std::tuple<std::decay_t<F>, std::tuple<std::decay_t<Args>...>>>(
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
              }
              else {
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


    template<std::ranges::input_range R, typename F, typename... Args>
    static auto enqueueBatch(R &&r, F &&f, Args &&... args) {
      return enqueueBatch<Priority::Normal>(std::forward<R>(r), std::forward<F>(f), std::forward<Args>(args)...);
    }

    template<Priority P, std::ranges::input_range R, typename F, typename... Args>
    static auto enqueueBatch(R &&r, F &&f, Args &&... args) {
      const auto count = r.size();
//      auto t = ThreadLog::time("Enqueue of {1} took {0}", count);

      using ArgType = std::ranges::range_value_t<R>;
      using return_type = std::invoke_result_t<F, int, ArgType, Args...>;
      constexpr bool is_void = std::is_void_v<return_type>;

      struct BatchState {
        std::atomic<size_t> remaining;
        std::promise<void> batch_promise;

        BatchState(size_t n) : remaining(n) {}
      };

      constexpr size_t SFO_LIMIT = 64;
      constexpr size_t SizeF = sizeof(std::decay_t<F>);
      constexpr size_t SharedArgsSize = (sizeof(std::decay_t<Args>) + ... + 0);
      constexpr size_t SizeElem = sizeof(ArgType);

      // Exact overhead based on what actually gets captured in the lambda
      constexpr size_t CAPTURE_OVERHEAD = is_void ? sizeof(std::shared_ptr<BatchState>)
                                                  : sizeof(std::promise<return_type>);

      std::vector<MoveOnlyFunction> batch;
      batch.reserve(count);

      // CASE 1: INDIVIDUAL CAPTURE (Fits in SFO)
      if constexpr (SizeF + SharedArgsSize + SizeElem + CAPTURE_OVERHEAD <= SFO_LIMIT) {
        if constexpr (is_void) {
          auto state = std::make_shared<BatchState>(count);


          for (auto &&item: r) {
            using ForwardType = std::conditional_t<std::is_lvalue_reference_v<R>,
                decltype(item) &,
                std::remove_reference_t<decltype(item)> &&>;
            batch.emplace_back([f, args..., arg = static_cast<ForwardType>(item), state](int id) mutable {
              try {
                f(id, std::move(arg), args...);
                if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) state->batch_promise.set_value();
              } catch (...) { state->batch_promise.set_exception(std::current_exception()); }
            });
          }
          pushBatch<P>(std::move(batch));
          return state->batch_promise.get_future();
        }
        else {
          std::vector<std::future<return_type>> futures;
          futures.reserve(count);
          for (auto &&item: r) {
            using ForwardType = std::conditional_t<std::is_lvalue_reference_v<R>,
                decltype(item) &,
                std::remove_reference_t<decltype(item)> &&>;
            std::promise<return_type> p;
            futures.push_back(p.get_future());
            batch.emplace_back(
                [f, ...args = args, arg = static_cast<ForwardType>(item), p = std::move(p)](int id) mutable {
                  try { p.set_value(f(id, std::move(arg), args...)); }
                  catch (...) { p.set_exception(std::current_exception()); }
                });
          }
          pushBatch<P>(std::move(batch));
          return futures;
        }
      }
        // CASE 2: MERGED STORAGE (Too big for SFO or Void-optimized)
      else {
        if constexpr (is_void) {
          // Capture everything in one shared allocation
          struct MergedState : BatchState {
            std::decay_t<R> payload;
            std::tuple<std::decay_t<Args>...> saved_args;

            MergedState(size_t n, R &&r, Args &&... a)
                : BatchState(n), payload(std::forward<R>(r)), saved_args(std::forward<Args>(a)...) {}
          };

          auto shared = std::make_shared<MergedState>(count, std::forward<R>(r), std::forward<Args>(args)...);

          for (size_t i = 0; i < count; ++i) {
            // Lambda now only captures one pointer (shared) and one index (i)
            batch.emplace_back([f, shared, i](int id) {
              try {
                std::apply([&](auto &&... unpacked_args) {
                  f(id, std::move(shared->payload[i]), unpacked_args...);
                }, shared->saved_args);

                if (shared->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                  shared->batch_promise.set_value();
              } catch (...) {
                shared->batch_promise.set_exception(std::current_exception());
              }
            });
          }
          pushBatch<P>(std::move(batch));
          return shared->batch_promise.get_future();
        }
        else {
          // Fallback for non-void remains mostly same, but keep in mind SFO pressure here too
          struct MergedNonVoidState {
            std::decay_t<R> payload;
            std::tuple<std::decay_t<Args>...> saved_args;

            MergedNonVoidState(R&& r, Args&&... a)
                : payload(std::forward<R>(r)),
                  saved_args(std::forward<Args>(a)...) {}
          };
          auto shared = std::make_shared<MergedNonVoidState>(
              std::forward<R>(r),
              std::forward<Args>(args)...
          );
          std::vector<std::future<return_type>> futures;
          futures.reserve(count);
          for (size_t i = 0; i < count; ++i) {
            std::promise<return_type> p;
            futures.push_back(p.get_future());
            batch.emplace_back([f, shared, i, p = std::move(p)](int id) mutable {
              try {
                std::apply([&](auto &&... unpacked) {
                  p.set_value(f(id, std::move(shared->payload[i]), unpacked...));
                }, shared->saved_args);
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
        }
        else {
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


  };
} // SC
