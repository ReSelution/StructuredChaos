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


    static void wait_until_finished();

    template<class F, class... Args>
    static auto enqueue(Priority p, F &&f, Args &&... args) -> std::future<std::invoke_result_t<F, Args...> > {
      using return_type = std::invoke_result_t<F, Args...>;

      auto task = std::make_shared<std::packaged_task<return_type()> >(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
      );

      std::future<return_type> res = task->get_future();

      pushTask(p, [task]() { (*task)(); });

      return res;
    }

    template<class F, class... Args>
    static auto enqueue(F &&f, Args &&... args) -> std::future<std::invoke_result_t<F, Args...> > {
      return enqueue(Priority::Normal, std::forward<F>(f), std::forward<Args>(args)...);
    }

    template<class F, class... Args>
    static auto enqueueLong(std::string_view name, F &&f, Args &&... args) -> std::future<std::invoke_result_t<F, Args...> > {
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

      pushLongTaskInternal(name,std::move(boundTask));
      return res;
    }

    template<class F, class... Args>
    static auto enqueueLong(F &&f, Args &&... args) -> std::future<std::invoke_result_t<F, Args...> > {
     return enqueueLong("ChaosLongTask", std::forward<F>(f), std::forward<Args>(args)...);
    }

  private:
    static void pushTask(Priority p, std::function<void(int)> task);

    static void pushLongTaskInternal(std::string_view name, std::function<void(std::stop_token)> task);
  };
} // SC
