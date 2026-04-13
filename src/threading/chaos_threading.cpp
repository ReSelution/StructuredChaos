#include "chaos_threading.hpp"
#include <vector>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic_queue/atomic_queue.h>

#include "tracy/Tracy.hpp"

#ifdef _WIN32
#include <windows.h>
#include <processthreadsapi.h>
#include <codecvt>
#include <locale>
#elif __linux__

#include <pthread.h>

#endif


namespace SC {
    struct ChaosTask {
        ChaosThreading::Priority priority;
        mutable MoveOnlyTask task;

        bool operator<(const ChaosTask &other) const {
            return priority < other.priority;
        }
    };


    using aQueue = atomic_queue::AtomicQueueB2<MoveOnlyTask, std::allocator<MoveOnlyTask>>;

    struct alignas(64) WorkerData {
        explicit WorkerData(uint32_t cap) : queue(cap) {}

        aQueue queue;
    };

    struct alignas(64) Queues {
        using Priority = ChaosThreading::Priority;
        std::array<aQueue, static_cast<size_t>(Priority::PriorityCount)> pQueues;

        Queues() : pQueues{aQueue(QUEUE_CAP), aQueue(QUEUE_CAP), aQueue(QUEUE_CAP)} {

        }

        bool try_pop_any(MoveOnlyTask &task) {
            return try_pop_impl(task, std::make_index_sequence<static_cast<size_t>(Priority::PriorityCount)>{});
        }

        [[nodiscard]] bool was_empty() const {
            return was_empty_impl(std::make_index_sequence<static_cast<size_t>(Priority::PriorityCount)>{});
        }

        template<Priority p>
        void enqueue(MoveOnlyTask &&task) {
            constexpr int index = static_cast<int>(p);
            pQueues[index].push(std::move(task));
        }

    private:

        template<size_t... Is>
        bool try_pop_impl(MoveOnlyTask &task, std::index_sequence<Is...>) {
            return (pQueues[Is].try_pop(task) || ...); // Fold Expression (C++17)
        }

        template<size_t... Is>
        [[nodiscard]] bool was_empty_impl(std::index_sequence<Is...>) const {
            return (pQueues[Is].was_empty() && ...);
        }
    };

    static std::mutex threadMutex;
    static std::vector<std::jthread> threads;
    static std::vector<std::jthread> longRunningThreads;

    static std::condition_variable_any cv;
    static std::condition_variable_any wait_cv;

    static Queues queues;
    static std::mutex queueMutex;

    static std::vector<std::unique_ptr<WorkerData>> workerStores;
    static std::atomic<uint32_t> available{0};

    static thread_local int32_t threadId = 0;

    void set_thread_name(std::string_view name) {
#ifdef _WIN32
        wchar_t wName[64];

        swprintf(wName, 64, L"%hs", name);
        SetThreadDescription(GetCurrentThread(), wName);
#elif __linux__
        char shortName[16];
        strncpy(shortName, name.data(), 15);
        shortName[15] = '\0';
        pthread_setname_np(pthread_self(), shortName);
#endif
    }

    size_t ChaosThreading::getNumThreads() {
        return threads.size();
    }

    int32_t ChaosThreading::getThreadId() {
        return threadId;
    }

    MoveOnlyTask helpThread(int id) {
        if (available.load(std::memory_order_acquire) == 0) return {};

        size_t numWorkers = workerStores.size();
        size_t startIdx = id % numWorkers;

        for (size_t i = 0; i < numWorkers; ++i) {
            size_t targetIdx = (startIdx + i) % numWorkers;
            auto &targetStore = workerStores[targetIdx];

            MoveOnlyTask task;
            if (targetStore->queue.try_pop(task)) {
                return task; // Sofort raus hier!
            }
        }
        return {};
    }

    void workerThread(const std::stop_token &st, int id) {
        char name[16];
        snprintf(name, sizeof(name), "ChaosPool-%d", id);
        //set_thread_name(name);
        threadId = id;
        //tracy::SetThreadName(name);
        while (!st.stop_requested()) {
            MoveOnlyTask task;


            if (queues.try_pop_any(task)) {
                ActiveTask::record(1);
                QueueSize::record(-1);
                task(id);
                ActiveTask::record(-1);
                CHAOS_RECORD(PoolThroughput, 1)
                continue;
            }

            task = helpThread(id);
            if (task) {
                task(id);
                continue;
            }


            if (queues.was_empty() && ActiveTask::m_storage.value.load() == 0) {
                wait_cv.notify_all();
            }
            {
                std::unique_lock lock(queueMutex);
                if (!cv.wait(lock, st,
                             [] { return !queues.was_empty() || available.load(std::memory_order_acquire) >= 1; }))
                    return;
            }

        }
    }

    void shutdown() {
        {
            std::lock_guard lock(threadMutex);

            for (auto &t: threads) {
                t.request_stop();
            }
            for (auto &t: longRunningThreads) {
                t.request_stop();
            }
        }
        cv.notify_all();
        threads.clear();
        longRunningThreads.clear();
    }

    void ChaosThreading::init(uint32_t numThreads) {
        if (!threads.empty()) return;
        auto maxThreads = std::thread::hardware_concurrency() - 1 - longRunningThreads.size();
        auto threadCount = std::min(std::max<size_t>(numThreads, 1), maxThreads);

        workerStores.reserve(threadCount);
        for (uint32_t i = 0; i < threadCount; ++i) {
            workerStores.push_back(std::make_unique<WorkerData>(threadCount * HELPER_TASK_MULTIPLYER));
        }

        CHAOS_START(PoolThroughput)
        std::atexit(shutdown);
        for (uint32_t i = 0; i < threadCount; ++i) {
            threads.emplace_back(workerThread, i + longRunningThreads.size());
        }
    }

    template<ChaosThreading::Priority P>
    void ChaosThreading::pushTask(MoveOnlyTask &&task) {
        queues.enqueue<P>(std::move(task));
        QueueSize::record(1);
        cv.notify_one();
    }

    template void ChaosThreading::pushTask<ChaosThreading::Priority::High>(MoveOnlyTask &&task);
    template void ChaosThreading::pushTask<ChaosThreading::Priority::Normal>(MoveOnlyTask &&task);
    template void ChaosThreading::pushTask<ChaosThreading::Priority::Low>(MoveOnlyTask &&task);


    template<ChaosThreading::Priority P>
    void ChaosThreading::pushBatch(std::vector<MoveOnlyTask> &batch) {

        for (auto &t: batch) {
            queues.enqueue<P>(std::move(t));
        }
        QueueSize::record(batch.size());
        cv.notify_all();
    }

    template void ChaosThreading::pushBatch<ChaosThreading::Priority::High>(std::vector<MoveOnlyTask>&);
    template void ChaosThreading::pushBatch<ChaosThreading::Priority::Normal>(std::vector<MoveOnlyTask>&);
    template void ChaosThreading::pushBatch<ChaosThreading::Priority::Low>(std::vector<MoveOnlyTask>&);

    void ChaosThreading::pushLongTaskInternal(std::string_view name, std::function<void(std::stop_token)> task) {
        if (!threads.empty()) {
            throw std::logic_error("Creating Long running Thread after init Call");
        }

        CHAOS_RECORD(LongRunningThreads, 1)
        std::lock_guard lock(threadMutex);
        std::erase_if(longRunningThreads, [](const std::jthread &t) { return !t.joinable(); });

        longRunningThreads.emplace_back([name, task = std::move(task)](const std::stop_token &st) {
            std::string nameStr(name);
            set_thread_name(nameStr);
            threadId = LongRunningThreads::m_storage.value;
            task(st);
            CHAOS_RECORD(LongRunningThreads, -1);
        });

    }

    void ChaosThreading::pushHelperTask(std::vector<MoveOnlyTask> &batch) {
        const auto id = getThreadId();
        auto &store = workerStores[id];
        for (auto &task: batch) {
            store->queue.push(std::move(task));
        }
        available.fetch_add(1);
        cv.notify_all();
        while (true) {
            MoveOnlyTask task;
            if (!store->queue.try_pop(task)) {
                available.fetch_sub(1);
                return;
            }
            task(id);
        }

    }

    void ChaosThreading::wait_until_finished() {
        std::unique_lock lock(queueMutex);
        wait_cv.wait(lock, [] {
            return QueueSize::m_storage.value.load() == 0 && ActiveTask::m_storage.value.load() == 0;
        });
    }
}
