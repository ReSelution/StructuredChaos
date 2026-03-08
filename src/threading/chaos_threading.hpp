//
// Created by oleub on 05.03.26.
//

#pragma once
#include <cstdint>
#include <functional>
#include <future>
#include <thread>

#include "stats/chaos_counter.hpp"
#include "stats/chaos_stats.hpp"
#include "stats/chaos_throughput.hpp"


DEFINE_CHAOS_STAT(PoolThroughput, "PoolTasks", SC::ChaosThroughput<SC::MetricUnits>);
DEFINE_CHAOS_STAT(QueueSize, "QueueDepth", SC::ChaosCounter<>);
DEFINE_CHAOS_STAT(ActiveTask, "ActiveTask", SC::ChaosCounter<>);
DEFINE_CHAOS_STAT(LongRunningThreads, "LongRunningThreads", SC::ChaosCounter<>);

REGISTER_CHAOS_STAT(PoolThroughput);
REGISTER_CHAOS_STAT(QueueSize);
REGISTER_CHAOS_STAT(ActiveTask);
REGISTER_CHAOS_STAT(LongRunningThreads);


namespace SC {
  class ChaosThreading {
  public:
    enum class Priority : int {
      Low = 0,
      Normal = 10,
      High = 20,
      Critical = 100
    };

    static void init(uint32_t numThreads = std::thread::hardware_concurrency() - 1);

    static size_t getNumThreads();

    static void wait_until_finished();

    template<class F, class... Args>
    requires std::invocable<F, int, Args...>
    static auto enqueue(Priority p, F &&f, Args &&... args) -> std::future<std::invoke_result_t<F, int, Args...> > {
      using return_type = std::invoke_result_t<F, int, Args...>;

      auto task = std::make_shared<std::packaged_task<return_type(int)> >(
        [f = std::forward<F>(f), ...args = std::forward<Args>(args)](int id) mutable {
          return f(id, std::forward<Args>(args)...);
        }
      );

      std::future<return_type> res = task->get_future();

      pushTask(p, [task](int id) { (*task)(id); });

      return res;
    }

    template<class F, class... Args>
    requires std::invocable<F, int, Args...>
    static auto enqueue(F &&f, Args &&... args) -> std::future<std::invoke_result_t<F, int, Args...> > {
      return enqueue(Priority::Normal, std::forward<F>(f), std::forward<Args>(args)...);
    }

    template<typename Iterator, typename F, typename... Args>
    static auto enqueueBatch(Priority p, Iterator begin, Iterator end, F &&f, Args &&... args) {
      using ArgType = std::iterator_traits<Iterator>::value_type;
      using return_type = std::invoke_result_t<F, int, ArgType, Args...>;

      auto shared_f = std::make_shared<std::decay_t<F> >(std::forward<F>(f));
      auto shared_args = std::make_shared<std::tuple<std::decay_t<Args>...> >(std::forward<Args>(args)...);

      std::vector<std::future<return_type> > futures;
      std::vector<std::function<void(int)> > batch;

      auto count = std::distance(begin, end);
      futures.reserve(count);
      batch.reserve(count);

      for (auto it = begin; it != end; ++it) {
        auto task = std::make_shared<std::packaged_task<return_type(int)> >(
          [shared_f, arg = std::move(*it), shared_args](int id) mutable {
            return std::apply([&](auto &&... unpacked_args) {
              return (*shared_f)(id, arg, unpacked_args...);
            }, *shared_args);
          }
        );

        futures.push_back(task->get_future());
        batch.emplace_back([task](int id) { (*task)(id); });
      }

      pushBatch(p, batch);

      return futures;
    }

    template<class F, class... Args>
    static auto enqueueLong(std::string_view name, F &&f,
                            Args &&... args) -> std::future<std::invoke_result_t<F, Args...> > {
      using return_type = std::invoke_result_t<F, std::stop_token, Args...>;

      auto promise = std::make_shared<std::promise<return_type> >();
      auto res = promise->get_future();

      auto boundTask = [promise, f = std::forward<F>(f), ...args = std::forward<Args>(args)
          ](std::stop_token st) mutable {
        try {
          if constexpr (std::is_void_v<return_type>) {
            f(st, std::forward<Args>(args)...);
            promise->set_value();
          } else {
            promise->set_value(f(st, std::forward<Args>(args)...));
          }
        } catch (...) {
          promise->set_exception(std::current_exception());
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
    static void pushTask(Priority p, std::function<void(int)> task);

    static void pushBatch(Priority p, std::span<std::function<void(int)> > batch);

    static void pushLongTaskInternal(std::string_view name, std::function<void(std::stop_token)> task);
  };
} // SC
