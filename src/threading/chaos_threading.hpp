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

    constexpr uint32_t HELPER_TASK_MULTIPLYER = 4;
    constexpr  uint32_t  QUEUE_CAP = 50000;

    struct MoveOnlyTask {
        struct Base {
            virtual ~Base() = default;

            virtual void call(int id) = 0;
        };

        template<typename F>
        struct Impl : Base {
            F f;

            Impl(F &&f) : f(std::forward<F>(f)) {
            }

            void call(int id) override { f(id); }
        };

        std::unique_ptr<Base> ptr;

        MoveOnlyTask() = default;

        template<typename F>
        requires (!std::is_same_v<std::decay_t<F>, MoveOnlyTask>)
        MoveOnlyTask(F &&f)
                : ptr(std::make_unique<Impl<std::decay_t<F> > >(std::forward<F>(f))) {
        }

        // 3. Move-only Logik
        MoveOnlyTask(MoveOnlyTask &&) noexcept = default;

        MoveOnlyTask &operator=(MoveOnlyTask &&) noexcept = default;

        MoveOnlyTask(const MoveOnlyTask &) = delete;

        MoveOnlyTask &operator=(const MoveOnlyTask &) = delete;

        void operator()(int id) const { if (ptr) ptr->call(id); }

        explicit operator bool() const { return ptr != nullptr; }
    };

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

        template<typename Iter, typename Func>
        static void parralelFor(Iter begin, Iter end, Func &&func) {
            auto totalSize = std::distance(begin, end);
            if (totalSize == 0) {
                return;
            }
            auto numTask = getNumThreads() * HELPER_TASK_MULTIPLYER;
            if (static_cast<size_t>(totalSize) < numTask)[[unlikely]] {
                numTask = static_cast<uint32_t>(totalSize);
            }
            auto chunkSize = totalSize / numTask;

            std::vector<MoveOnlyTask> batch;
            batch.reserve(numTask);

            struct alignas(64) BatchContext {
                std::atomic<uint32_t> remaining;
                std::promise<void> promise;

                BatchContext(uint32_t count) : remaining(count) {}
            };

            auto ctx = std::make_unique<BatchContext>(numTask);
            auto ftr = ctx->promise.get_future();
            auto rawCtx = ctx.get();

            auto current = begin;
            for (uint32_t i = 0; i < numTask; ++i) {
                auto next = current;
                if (i == numTask - 1) {
                    next = end; // Den Rest einpacken
                } else {
                    std::advance(next, chunkSize);
                }
                batch.emplace_back([current, next, rawCtx, &func](int id) {
                    for (auto it = current; it != next; ++it) {
                        if constexpr (requires { std::apply(func, *it); }) {
                            std::apply(func, *it);
                        } else {
                            func(*it);
                        }
                    }
                    if (rawCtx->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        rawCtx->promise.set_value();
                    }
                });
                current = next;
            }
            pushHelperTask(batch);
            ftr.wait();

        };


        template< class F, class... Args>
        requires std::invocable<F, int, Args...>
        static auto enqueue(F &&f, Args &&... args) -> std::future<std::invoke_result_t<F, int, Args...> > {
            return enqueue<Priority::Normal>(std::forward<F>(f), std::forward<Args>(args) ...);
        }


        template<Priority P , class F, class... Args>
        requires std::invocable<F, int, Args...>
        static auto enqueue(F &&f, Args &&... args) -> std::future<std::invoke_result_t<F, int, Args...> > {
            using return_type = std::invoke_result_t<F, int, Args...>;

            auto task = std::make_unique<std::packaged_task<return_type(int)> >(
                    [f = std::forward<F>(f), ...args = std::forward<Args>(args)](int id) mutable {
                        return f(id, std::forward<Args>(args)...);
                    }
            );

            std::future<return_type> res = task->get_future();

            pushTask<P>([task = std::move(task)](int id) { (*task)(id); });

            return res;
        }

//        template<class F, class... Args>
//        requires std::invocable<F, int, Args...>
//        static auto enqueue(F &&f, Args &&... args) -> std::future<std::invoke_result_t<F, int, Args...> > {
//            return enqueue( std::forward<F>(f), std::forward<Args>(args)...);
//        }

        template<Priority P = Priority::Normal, typename Iterator, typename F, typename... Args>
        static auto enqueueBatch(Iterator begin, Iterator end, F &&f, Args &&... args) {
            using ArgType = std::iterator_traits<Iterator>::value_type;
            using return_type = std::invoke_result_t<F, int, ArgType, Args...>;
            using IterRef = decltype(*std::declval<Iterator &>());

            auto shared_f = std::make_shared<std::decay_t<F> >(std::forward<F>(f));
            auto shared_args = std::make_shared<std::tuple<std::decay_t<Args>...> >(std::forward<Args>(args)...);

            std::vector<std::future<return_type> > futures;
            std::vector<MoveOnlyTask> batch;

            auto count = std::distance(begin, end);
            futures.reserve(count);
            batch.reserve(count);

            for (auto it = begin; it != end; ++it) {
                auto task = std::make_unique<std::packaged_task<return_type(int)> >(
                        [shared_f, arg = static_cast<IterRef>(*it), shared_args](int id) mutable {
                            try {
                                return std::apply([&](auto &&... unpacked_args) {
                                    return (*shared_f)(id, arg, unpacked_args...);
                                }, *shared_args);
                            } catch (std::exception &e) {
                                auto safe_arg = [&]() {
                                    if constexpr (std::is_pointer_v<ArgType>) {
                                        return static_cast<const void *>(arg);
                                    } else {
                                        return arg;
                                    }
                                }();
                                ThreadLog::err("Exception: Argument-Value: {} Error: {}", safe_arg, e.what());
                            }
                        }
                );

                futures.push_back(task->get_future());
                batch.emplace_back([task = std::move(task)](int id) { (*task)(id); });
            }

            pushBatch<P>( batch);

            return futures;
        }

        template<class F, class... Args>
        static auto enqueueLong(std::string_view name, F &&f,
                                Args &&... args) -> std::future<std::invoke_result_t<F, Args...> > {
            using return_type = std::invoke_result_t<F, std::stop_token, Args...>;

            auto promise = std::make_unique<std::promise<return_type> >();
            auto res = promise->get_future();

            auto boundTask = [promise = std::move(promise), f = std::forward<F>(f), ...args = std::forward<Args>(args)
            ](std::stop_token st) mutable {
                if constexpr (std::is_void_v<return_type>) {
                    f(st, std::move(args)...);
                    promise->set_value();
                } else {
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
        static void pushTask(MoveOnlyTask &&task);

        template<Priority P>
        static void pushBatch( std::vector<MoveOnlyTask> &batch);

        static void pushLongTaskInternal(std::string_view name, std::function<void(std::stop_token)> task);

        static void pushHelperTask(std::vector<MoveOnlyTask> &batch);
    };
} // SC
