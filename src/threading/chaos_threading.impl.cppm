module;

#include <array>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <ranges>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>


#ifdef _WIN32
#include <processthreadsapi.h>
#include <windows.h>
#elif __linux__
#include <cstring>
#include <pthread.h>
#endif

#include <atomic_queue/atomic_queue.h>

export module SC.Threading:Impl;
import SC.Stats;
import SC.Logger;

namespace SC {

  // ============================================================================
  // C++20 Stats Definitionen
  // ============================================================================
  export using QueueSize          = SC::ChaosStat<"QueueDepth", SC::ChaosCounter<>>;
  export using ActiveTask         = SC::ChaosStat<"ActiveTask", SC::ChaosCounter<>>;
  export using PoolThroughput     = SC::ChaosStat<"PoolTasks", SC::ChaosThroughput<SC::MetricUnits>>;
  export using LongRunningThreads = SC::ChaosStat<"LongRunningThreads", SC::ChaosCounter<>>;

  constexpr uint64_t HELPER_TASK_MULTIPLYER = 4;
  constexpr uint64_t QUEUE_CAP              = 50000;
  using ThreadLog                           = ChaosLogger<"Chaos", "Threading">;

  export using MoveOnlyFunction = std::move_only_function<void(int)>;

  namespace Detail {
    struct BatchState {
      std::atomic<size_t> remaining;
      std::promise<void> batch_promise;
      BatchState(size_t n) : remaining(n) {}
    };

    template<typename R, typename... Args>
    struct MergedState : BatchState {
      std::decay_t<R> payload;
      std::tuple<std::decay_t<Args>...> saved_args;
      MergedState(size_t n, R &&r, Args &&...a) :
          BatchState(n), payload(std::forward<R>(r)), saved_args(std::forward<Args>(a)...) {}
    };

    struct DetachBatchState {
      std::atomic<size_t> remaining;
      MoveOnlyFunction on_finished;
      DetachBatchState(size_t n, MoveOnlyFunction &&cb) : remaining(n), on_finished(std::move(cb)) {}
    };

    template<typename R, typename... Args>
    struct DetachedMergedState : DetachBatchState {
      std::decay_t<R> payload;
      std::tuple<std::decay_t<Args>...> saved_args;
      DetachedMergedState(size_t n, R &&r, MoveOnlyFunction &&cb, Args &&...a) :
          DetachBatchState(n, std::move(cb)), payload(std::forward<R>(r)), saved_args(std::forward<Args>(a)...) {}
    };

    template<typename R, typename... Args>
    struct MergedNonVoidState {
      std::decay_t<R> payload;
      std::tuple<std::decay_t<Args>...> saved_args;
      MergedNonVoidState(R &&r, Args &&...a) : payload(std::forward<R>(r)), saved_args(std::forward<Args>(a)...) {}
    };

    struct BatchContext {
      alignas(64) std::atomic<uint32_t> remaining;
      BatchContext(uint32_t count) : remaining(count) {}
    };
  } // namespace Detail

  using aQueue = atomic_queue::AtomicQueueB2<MoveOnlyFunction, std::allocator<MoveOnlyFunction>>;

  struct alignas(64) WorkerData {
    explicit WorkerData(uint32_t cap) : queue(cap) {}
    aQueue queue;
  };

  export class ChaosThreading {
    static constexpr size_t SFO_LIMIT = 64;

  public:
    enum class Priority : int32_t {
      High,
      Normal,
      Low,
      PriorityCount,
    };

  private:
    struct Queues {
      std::array<aQueue, static_cast<size_t>(Priority::PriorityCount)> pQueues;
      Queues() : pQueues{aQueue(QUEUE_CAP), aQueue(QUEUE_CAP), aQueue(QUEUE_CAP)} {}

      bool try_pop_any(MoveOnlyFunction &task) {
        return (pQueues[0].try_pop(task) || pQueues[1].try_pop(task) || pQueues[2].try_pop(task));
      }

      [[nodiscard]] bool was_empty() const {
        return (pQueues[0].was_empty() && pQueues[1].was_empty() && pQueues[2].was_empty());
      }

      template<Priority p>
      void enqueue(MoveOnlyFunction &&task) {
        pQueues[static_cast<size_t>(p)].push(std::move(task));
      }
    };

    static inline std::mutex threadMutex;
    static inline std::vector<std::jthread> threads;
    static inline std::vector<std::jthread> longRunningThreads;
    static inline std::condition_variable_any wait_cv;

    static constexpr size_t MAX_TASKS = QUEUE_CAP * static_cast<size_t>(Priority::PriorityCount);
    static inline std::counting_semaphore<MAX_TASKS> pool_sema{0};

    static inline Queues queues;
    static inline std::mutex queueMutex;
    static inline std::vector<std::unique_ptr<WorkerData>> workerStores;
    static inline std::atomic<uint64_t> available{0};
    static inline thread_local int32_t threadId = 0;

    static void set_thread_name([[maybe_unused]] std::string_view name) {
#ifdef _WIN32
      wchar_t wName[64];
      swprintf(wName, 64, L"%hs", name.data());
      SetThreadDescription(GetCurrentThread(), wName);
#elif __linux__
      char shortName[16];
      std::strncpy(shortName, name.data(), 15);
      shortName[15] = '\0';
      pthread_setname_np(pthread_self(), shortName);
#endif
    }

    static void workerThread(const std::stop_token &st, int id) {
      threadId = id;
      while (!st.stop_requested()) {
        MoveOnlyFunction task;
        if (queues.try_pop_any(task)) {
          ActiveTask::record(1);
          QueueSize::record(-1);
          pool_sema.try_acquire();

          task(id);

          ActiveTask::record(-1);
          PoolThroughput::record(1);
          continue;
        }
        task = helpThread(id);
        if (task) {
          pool_sema.try_acquire();
          task(id);
          continue;
        }

        if (queues.was_empty() && ActiveTask::m_storage.value.load(std::memory_order_relaxed) == 0) [[unlikely]] {
          wait_cv.notify_all();
        }
        pool_sema.try_acquire_for(std::chrono::milliseconds(10));
      }
    }

    static void signalWork(size_t count) { pool_sema.release(count); }

    static void shutdown() {
      {
        std::lock_guard lock(threadMutex);
        for (auto &t: threads)
          t.request_stop();
        for (auto &t: longRunningThreads)
          t.request_stop();
      }
      signalWork(threads.size());
      threads.clear();
      longRunningThreads.clear();
    }

  public:
    static void init(uint32_t numThreads = std::thread::hardware_concurrency() - 1) {
      if (!threads.empty()) [[unlikely]]
        return;

      const size_t longCount   = longRunningThreads.size();
      const size_t maxHardware = std::thread::hardware_concurrency();
      const size_t threadCount = std::min<size_t>(numThreads, maxHardware - 1 - longCount);
      const size_t totalSlots  = 1 + longCount + threadCount;

      workerStores.reserve(totalSlots);
      for (uint32_t i = 0; i < totalSlots; ++i) {
        workerStores.push_back(std::make_unique<WorkerData>(QUEUE_CAP));
      }

      std::atexit(shutdown);
      for (uint32_t i = 0; i < threadCount; ++i) {
        threads.emplace_back(workerThread, i + 1 + longRunningThreads.size());
      }
      threadId = 0;
    }

    [[nodiscard]] static size_t getNumThreads() noexcept { return threads.size() + 1 + longRunningThreads.size(); }

    [[nodiscard]] static int32_t getThreadId() noexcept { return threadId; }

    static void wait_until_finished() {
      std::unique_lock lock(queueMutex);
      wait_cv.wait(lock, [] {
        return QueueSize::m_storage.value.load(std::memory_order_relaxed) == 0 &&
               ActiveTask::m_storage.value.load(std::memory_order_relaxed) == 0;
      });
    }

    template<std::ranges::input_range R, typename Func>
    static void parallelFor(R &&range, Func &&func) {
      auto totalSize = std::ranges::distance(range);
      if (totalSize <= 0) [[unlikely]]
        return;

      const size_t threadLimit   = getNumThreads() * HELPER_TASK_MULTIPLYER;
      const size_t numTasksLimit = std::min<size_t>(totalSize, threadLimit);
      const size_t chunkSize     = totalSize / numTasksLimit;

      Detail::BatchContext ctx{static_cast<uint32_t>(numTasksLimit)};
      auto rawCtx = &ctx;

      auto current = range.begin();
      auto end     = range.end();
      for (uint32_t i = 0; i < numTasksLimit; ++i) {
        auto next = current;
        if (i == numTasksLimit - 1) {
          next = end;
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
    }

    template<typename F, typename... Args>
      requires std::invocable<F, int, Args...> && std::is_void_v<std::invoke_result_t<F, int, Args...>>
    static void detach(F &&f, Args &&...args) {
      detach<Priority::Normal>(std::forward<F>(f), std::forward<Args>(args)...);
    }

    template<Priority P, typename F, typename... Args>
      requires std::invocable<F, int, Args...> && std::is_void_v<std::invoke_result_t<F, int, Args...>>
    static void detach(F &&f, Args &&...args) {
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

    template<typename F, typename... Args>
      requires std::invocable<F, int, Args...>
    static auto enqueue(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, int, Args...>> {
      return enqueue<Priority::Normal>(std::forward<F>(f), std::forward<Args>(args)...);
    }

    template<Priority P, typename F, typename... Args>
      requires std::invocable<F, int, Args...>
    static auto enqueue(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, int, Args...>> {
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


    template<std::ranges::input_range R, typename F, typename Finished, typename... Args>
      requires std::invocable<F, int, std::ranges::range_value_t<R>, Args...> &&
               std::is_void_v<std::invoke_result_t<F, int, std::ranges::range_value_t<R>, Args...>>
    static void detachBatch(R &&r, F &&f, Finished &&finished, Args &&...args) {
      detachBatch<Priority::Normal>(std::forward<R>(r), std::forward<F>(f), std::forward<Finished>(finished),
                                    std::forward<Args>(args)...);
    }

    template<Priority P, std::ranges::input_range R, typename F, typename Finished, typename... Args>
      requires std::invocable<F, int, std::ranges::range_value_t<R>, Args...> &&
               std::is_void_v<std::invoke_result_t<F, int, std::ranges::range_value_t<R>, Args...>>
    static void detachBatch(R &&r, F &&f, Finished &&finished, Args &&...args) {
      const auto count = r.size();
      if (count == 0) {
        return;
      }
      using ArgType = std::ranges::range_value_t<R>;

      constexpr size_t SFO_LIMIT = 64;
      constexpr size_t CAPTURE_BASE =
          sizeof(std::decay_t<F>) + (0 + ... + sizeof(std::decay_t<Args>)) + sizeof(ArgType);
      constexpr size_t OVERHEAD = sizeof(std::shared_ptr<void>);

      std::vector<MoveOnlyFunction> batch;
      batch.reserve(count);
      auto arg_tuple              = std::make_tuple(std::forward<Args>(args)...);
      constexpr bool is_null_type = std::is_same_v<std::remove_cvref_t<Finished>, std::nullptr_t>;

      if constexpr (CAPTURE_BASE + OVERHEAD <= SFO_LIMIT || (!is_null_type && CAPTURE_BASE <= SFO_LIMIT)) {
        if constexpr (is_null_type) {
          for (auto &&item: r) {
            batch.emplace_back([f, args..., arg = std::move(item)](int id) mutable {
              try {
                f(id, std::move(arg), args...);
              } catch (const std::exception &e) {
                ThreadLog::err("Exception: {}", e.what());
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
                  if (state->on_finished)
                    state->on_finished(id);
                }
              } catch (...) {
                if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                  if (state->on_finished)
                    state->on_finished(id);
                }
              }
            });
          }
        }
        pushBatch<P>(std::move(batch));
      } else {
        if constexpr (is_null_type) {
          // CASE: Kein Callback -> Nutze Standard MergedState
          auto shared =
              std::make_shared<Detail::MergedState<R, Args...>>(count, std::forward<R>(r), std::forward<Args>(args)...);

          for (size_t i = 0; i < count; ++i) {
            batch.emplace_back([f, shared, i](int id) {
              try {
                std::apply([&](auto &&...unpacked) { f(id, std::move(shared->payload[i]), unpacked...); },
                           shared->saved_args);

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

            DetachedMergedState(size_t n, R &&r, Finished &&cb, Args &&...a) :
                Detail::MergedState<R, Args...>(n, std::forward<R>(r), std::forward<Args>(a)...),
                on_finished(std::forward<Finished>(cb)) {}
          };

          auto shared = std::make_shared<DetachedMergedState>(
              count, std::forward<R>(r), std::forward<Finished>(finished), std::forward<Args>(args)...);

          for (size_t i = 0; i < count; ++i) {
            batch.emplace_back([f, shared, i](int id) {
              try {
                std::apply([&](auto &&...unpacked) { f(id, std::move(shared->payload[i]), unpacked...); },
                           shared->saved_args);

                if (shared->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                  if (shared->on_finished)
                    shared->on_finished(id);
                  shared->batch_promise.set_value();
                }
              } catch (...) {
                shared->batch_promise.set_exception(std::current_exception());
                if (shared->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                  if (shared->on_finished)
                    shared->on_finished(id);
                }
              }
            });
          }
        }
        pushBatch<P>(std::move(batch));
      }
    }

    template<std::ranges::input_range R, typename F, typename... Args>
    static auto enqueueBatch(R &&r, F &&f, Args &&...args) {
      return enqueueBatch<Priority::Normal>(std::forward<R>(r), std::forward<F>(f), std::forward<Args>(args)...);
    }

    template<Priority P, std::ranges::input_range R, typename F, typename... Args>
      requires std::invocable<F, int, std::ranges::range_value_t<R>, Args...>
    static auto enqueueBatch(R &&r, F &&f, Args &&...args) {
      const size_t count     = r.size();
      using ArgType          = std::ranges::range_value_t<R>;
      using RetType          = std::invoke_result_t<F, int, ArgType, Args...>;
      constexpr bool is_void = std::is_void_v<RetType>;

      constexpr size_t SFO_LIMIT = 64;
      constexpr size_t CAPTURE_BASE =
          sizeof(std::decay_t<F>) + (0 + ... + sizeof(std::decay_t<Args>)) + sizeof(ArgType);
      constexpr size_t OVERHEAD = is_void ? sizeof(std::shared_ptr<void>) : sizeof(std::promise<RetType>);

      std::vector<MoveOnlyFunction> batch;
      batch.reserve(count);
      auto arg_tuple = std::make_tuple(std::forward<Args>(args)...);

      // CASE 1: SFO (Fast-Path)
      if constexpr (CAPTURE_BASE + OVERHEAD <= SFO_LIMIT) {
        if constexpr (is_void) {
          auto state = std::make_shared<Detail::BatchState>(count);
          for (auto &&item: r) {
            using ForwardType = std::conditional_t<std::is_lvalue_reference_v<R>, decltype(item) &,
                                                   std::remove_reference_t<decltype(item)> &&>;
            batch.emplace_back(make_void_individual(f, static_cast<ForwardType>(item), arg_tuple, state));
          }
          pushBatch<P>(std::move(batch));
          return state->batch_promise.get_future();
        } else {
          std::vector<std::future<RetType>> futures;
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
          auto shared =
              std::make_shared<Detail::MergedState<R, Args...>>(count, std::forward<R>(r), std::forward<Args>(args)...);
          for (size_t i = 0; i < count; ++i)
            batch.emplace_back(make_void_merged(f, shared, i));
          pushBatch<P>(std::move(batch));
          return shared->batch_promise.get_future();
        } else {
          auto shared =
              std::make_shared<Detail::MergedNonVoidState<R, Args...>>(std::forward<R>(r), std::forward<Args>(args)...);
          std::vector<std::future<RetType>> futures;
          futures.reserve(count);
          for (size_t i = 0; i < count; ++i) {
            std::promise<RetType> p;
            futures.push_back(p.get_future());
            batch.emplace_back([f, shared, i, p = std::move(p)](int id) mutable {
              try {
                std::apply([&](auto &&...ex) { p.set_value(f(id, std::move(shared->payload[i]), ex...)); },
                           shared->saved_args);
              } catch (...) {
                p.set_exception(std::current_exception());
              }
            });
          }
          pushBatch<P>(std::move(batch));
          return futures;
        }
      }
    }

    template<typename F, typename... Args>
    static auto enqueueLong(std::string_view name, F &&f, Args &&...args)
        -> std::future<std::invoke_result_t<F, std::stop_token, Args...>> {
      using return_type = std::invoke_result_t<F, std::stop_token, Args...>;
      std::promise<return_type> promise;
      auto res = promise.get_future();

      auto boundTask = [p = std::move(promise), f = std::forward<F>(f),
                        ... args = std::forward<Args>(args)](std::stop_token st) mutable {
        if constexpr (std::is_void_v<return_type>) {
          f(st, std::move(args)...);
          p.set_value();
        } else {
          p.set_value(f(st, std::move(args)...));
        }
      };
      pushLongTaskInternal(name, std::move(boundTask));
      return res;
    }

    template<class F, class... Args>
    static auto enqueueLong(F &&f, Args &&...args) {
      return enqueueLong("ChaosLongTask", std::forward<F>(f), std::forward<Args>(args)...);
    }

    static void doWork() {
      const auto id = getThreadId();
      auto &store   = workerStores[id];
      MoveOnlyFunction task;
      while (store->queue.try_pop(task)) {
        pool_sema.try_acquire();
        task(id);
      }
      available.fetch_and(~(1u << id), std::memory_order_release);
    }

  private:
    static MoveOnlyFunction helpThread(int id) {
      uint64_t mask = available.load(std::memory_order_acquire);
      if (mask == 0)
        return {};

      const auto n     = workerStores.size();
      uint32_t shift   = (id + 1) % n;
      uint64_t rotated = (mask >> shift) | (mask << (n - shift));
      rotated &= (1ULL << n) - 1;

      int rotatedIdx     = std::countr_zero(rotated);
      uint32_t targetIdx = (rotatedIdx + shift) % n;

      MoveOnlyFunction task;
      if (workerStores[targetIdx]->queue.try_pop(task))
        return task;
      return {};
    }

    template<Priority P>
    static void pushTask(MoveOnlyFunction &&task) {
      queues.enqueue<P>(std::move(task));
      QueueSize::record(1);
      signalWork(1);
    }

    template<Priority P>
    static void pushBatch(std::vector<MoveOnlyFunction> &&batch) {
      for (auto &it: batch)
        queues.enqueue<P>(std::move(it));
      QueueSize::record(batch.size());
      signalWork(batch.size());
    }

    static void pushLongTaskInternal(std::string_view name, std::function<void(std::stop_token)> task) {
      if (!threads.empty())
        throw std::logic_error("Creating Long running Thread after init Call");

      LongRunningThreads::record(1);
      std::lock_guard lock(threadMutex);
      std::erase_if(longRunningThreads, [](const std::jthread &t) { return !t.joinable(); });

      longRunningThreads.emplace_back([name, task = std::move(task)](const std::stop_token &st) {
        std::string nameStr(name);
        set_thread_name(nameStr);
        task(st);
        LongRunningThreads::record(-1);
      });
    }

    static void pushHelperTask(MoveOnlyFunction &&task) {
      const auto id = getThreadId();
      auto &store   = workerStores[id];
      store->queue.push(std::move(task));
      available.fetch_or(1u << id, std::memory_order_release);
      signalWork(1);
    }

    // --- VOID PATHS ---
    template<typename F, typename Arg, typename... Args>
    static auto make_void_individual(F &&f, Arg &&item, const std::tuple<Args...> &args,
                                     std::shared_ptr<Detail::BatchState> &state) {
      return [f, args, item = std::forward<Arg>(item), state](int id) mutable {
        try {
          std::apply([&](auto &&...extra) { f(id, std::move(item), extra...); }, args);
          if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
            state->batch_promise.set_value();
        } catch (...) {
          state->batch_promise.set_exception(std::current_exception());
        }
      };
    }

    template<typename F, typename Arg, typename... Args>
    static auto make_void_individualWithCallback(F &&f, Arg &&item, const std::tuple<Args...> &args,
                                                 std::shared_ptr<Detail::DetachBatchState> &state) {
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
    static auto make_void_merged(F &&f, Shared shared, size_t i) {
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
    static auto make_nonvoid_individual(F &&f, Arg &&item, const std::tuple<Args...> &args, Promise p) {
      return [f, args, item = std::forward<Arg>(item), p = std::move(p)](int id) mutable {
        try {
          std::apply([&](auto &&...extra) { p.set_value(f(id, std::move(item), extra...)); }, args);
        } catch (...) {
          p.set_exception(std::current_exception());
        }
      };
    }
  };

} // namespace SC
