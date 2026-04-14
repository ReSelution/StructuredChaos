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


    template<class F, class... Args>
    requires std::invocable<F, int, Args...>
    static auto enqueue(F &&f, Args &&... args) -> std::future<std::invoke_result_t<F, int, Args...> > {
      return enqueue<Priority::Normal>(std::forward<F>(f), std::forward<Args>(args) ...);
    }


    template<Priority P, class F, class... Args>
    requires std::invocable<F, int, Args...>
    static auto enqueue(F &&f, Args &&... args) -> std::future<std::invoke_result_t<F, int, Args...> > {
      using return_type = std::invoke_result_t<F, int, Args...>;

      auto task = std::packaged_task<return_type(int)>(
          [f = std::forward<F>(f), ...args = std::forward<Args>(args)](int id) mutable {
            return f(id, std::forward<Args>(args)...);
          }
      );

      std::future<return_type> res = task.get_future();

      pushTask<P>([task = std::move(task)](int id) { task(id); });

      return res;
    }


    template<std::ranges::input_range R, typename F, typename... Args>
    static auto enqueueBatch(R &&r, F &&f, Args &&... args) {
      return enqueueBatch<Priority::Normal>(std::forward<R>(r), std::forward<F>(f), std::forward<Args>(args)...);
    }

    template<Priority P, std::ranges::input_range R, typename F, typename... Args>
    static auto enqueueBatch(R &&r, F &&f, Args &&... args) {
      const auto count = r.size();
      auto t = ThreadLog::time("Enqueue of {1} took {0}", count);

      using ArgType =std::ranges::range_value_t<R>;
      using return_type = std::invoke_result_t<F, int, ArgType, Args...>;

      constexpr size_t SFO_LIMIT = 64;
      constexpr size_t SizeF = sizeof(std::decay_t<F>);
      constexpr size_t SharedArgsSize = (sizeof(std::decay_t<Args>) + ... + 0);
      constexpr size_t SizeElem = sizeof(ArgType);

      // Minimaler Overhead: 8 Bytes für den Promise-State/Pointer
      constexpr size_t MIN_OVERHEAD = 8;

      std::vector<std::future<return_type>> futures;
      futures.reserve(count);
      std::vector<MoveOnlyFunction> batch;
      batch.reserve(count);


// --- CASE 1: INDIVIDUAL CAPTURE (Maximale Performance, Zero Shared State) ---
      // Alles passt direkt in das Lambda-Objekt. Kein Shared_ptr nötig.
      if constexpr (SizeF + SharedArgsSize + SizeElem + MIN_OVERHEAD <= SFO_LIMIT) {
        for (auto&& item : r) {
          std::promise<return_type> promise;
          futures.push_back(promise.get_future());

          batch.emplace_back([f, args..., arg = std::forward<decltype(item)>(item), p = std::move(promise)](int id) mutable {
            try {
              if constexpr (std::is_void_v<return_type>) {
                f(id, std::move(arg), args...);
                p.set_value();
              } else {
                p.set_value(f(id, std::move(arg), args...));
              }
            } catch (...) {
              p.set_exception(std::current_exception());
            }
          });
        }
      }
        // --- CASE 2: SHARED PAYLOAD (Payload zu groß, aber F + Args passen) ---
        // Der klassische Fall für schwere Komponenten-Daten.
      else if constexpr (SizeF + SharedArgsSize + 16 + MIN_OVERHEAD <= SFO_LIMIT) {
        auto shared_payload = std::make_shared<std::decay_t<R>>(std::forward<R>(r));

        for (size_t i = 0; i < count; ++i) {
          std::promise<return_type> promise;
          futures.push_back(promise.get_future());

          batch.emplace_back([f, args..., shared_payload, i, p = std::move(promise)](int id) mutable {
            try {
              ArgType my_arg = std::move((*shared_payload)[i]);
              if constexpr (std::is_void_v<return_type>) {
                f(id, std::move(my_arg), args...);
                p.set_value();
              } else {
                p.set_value(f(id, std::move(my_arg), args...));
              }
            } catch (...) {
              p.set_exception(std::current_exception());
            }
          });
        }
      }
        // --- CASE 3: FULL SHARED CONTEXT (Nichts passt ins SFO) ---
        // Notfall-Plan: Alles in eine einzige Heap-Allokation auslagern.
      else {
        auto shared_ctx = std::make_shared<std::tuple<std::decay_t<F>, std::tuple<std::decay_t<Args>...>, std::decay_t<R>>>(
            std::forward<F>(f),
            std::make_tuple(std::forward<Args>(args)...),
            std::forward<R>(r)
        );

        for (size_t i = 0; i < count; ++i) {
          std::promise<return_type> promise;
          futures.push_back(promise.get_future());

          batch.emplace_back([shared_ctx, i, p = std::move(promise)](int id) mutable {
            try {
              auto& func = std::get<0>(*shared_ctx);
              auto& base_args = std::get<1>(*shared_ctx);
              auto& payload_vec = std::get<2>(*shared_ctx);
              ArgType my_arg = std::move(payload_vec[i]);

              std::apply([&](auto&&... unpacked) {
                if constexpr (std::is_void_v<return_type>) {
                  func(id, std::move(my_arg), std::forward<decltype(unpacked)>(unpacked)...);
                  p.set_value();
                } else {
                  p.set_value(func(id, std::move(my_arg), std::forward<decltype(unpacked)>(unpacked)...));
                }
              }, base_args);
            } catch (...) {
              p.set_exception(std::current_exception());
            }
          });
        }
      }

      pushBatch<P>(std::move(batch));
      return futures;
    }

    template<class F, class... Args>
    static auto
    enqueueLong(std::string_view name, F &&f, Args &&... args) -> std::future<std::invoke_result_t<F, Args...> > {
      using return_type = std::invoke_result_t<F, std::stop_token, Args...>;

      auto promise = std::make_unique<std::promise<return_type> >();
      auto res = promise->get_future();

      auto boundTask = [promise = std::move(promise), f = std::forward<F>(f), ...args = std::forward<Args>(args)
      ](std::stop_token st) mutable {
        if constexpr (std::is_void_v<return_type>) {
          f(st, std::move(args)...);
          promise->set_value();
        }
        else {
          promise->set_value(f(st, std::move(args)...));
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
