#include "background.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <semaphore>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sc/logger/logger.hpp"

#include "backend.hpp"
#include "ring_queue.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace sc::threading {

  namespace internal {
    constinit thread_local bool backgroundThread = false;
  } // namespace internal

  namespace {
    // Jobs that were put on the lane and have not gone yet.
    std::atomic<size_t> outstanding{0};

    // Counts a job out once it goes: first in QueuedJob, so it is destroyed
    // after the job, and whoever waits may rely on its captures being gone.
    class Outstanding {
    public:
      Outstanding() = default;
      explicit Outstanding(bool counted) noexcept : m_counted(counted) {}
      Outstanding(Outstanding &&other) noexcept : m_counted(std::exchange(other.m_counted, false)) {}
      Outstanding &operator=(Outstanding &&other) noexcept {
        if (this != &other) {
          finish();
          m_counted = std::exchange(other.m_counted, false);
        }
        return *this;
      }
      Outstanding(const Outstanding &) = delete;
      Outstanding &operator=(const Outstanding &) = delete;
      ~Outstanding() { finish(); }

    private:
      void finish() noexcept {
        if (std::exchange(m_counted, false) && outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          outstanding.notify_all();
        }
      }

      bool m_counted = false;
    };

    struct QueuedJob {
      Outstanding counted;
      internal::BackgroundJob job;
      // Without a state of its own: a default constructed stop_source
      // allocates one, and every turn of a thread of the lane makes a job.
      std::stop_source stop{std::nostopstate};
    };

    // A spawn blocks (spinning) while the queue is full. 64 bytes a slot
    // (clang, libstdc++), so 4 MB of address space, touched only where a
    // slot is used; on Windows they count against the commit limit, see
    // RingQueue.
    inline constexpr size_t LANE_CAP = size_t{1} << 16;

    // Without a lock: a thread of the lane runs at SCHED_IDLE, and a system
    // that waited in spawn for a lock such a thread holds would wait until
    // the thread gets a core.
    RingQueue<QueuedJob, LANE_CAP> laneQueue;
    // One release per job, as poolSemaphore.
    std::counting_semaphore<LANE_CAP> laneSemaphore{0};

    // Guards laneThreads in initBackground and shutdownBackground.
    std::mutex laneThreadMutex;
    std::vector<std::jthread> laneThreads;
    // laneThreads.size(), for readers that must not take the mutex.
    std::atomic<size_t> laneThreadCount{0};

    inline constexpr uint64_t LANE_IDLE = std::numeric_limits<uint64_t>::max();

    // Raised by every markBackground.
    std::atomic<uint64_t> laneEpoch{0};

    // The epoch at which the job of a thread of the lane started.
    struct alignas(64) LaneSlot {
      std::atomic<uint64_t> since{LANE_IDLE};
      std::atomic<bool> taken{false};
    };

    // Guarded by laneThreadMutex; a deque, so that a thread keeps its slot
    // while others are added. Slots outlive laneThreads: shutdownBackground
    // joins after it has emptied that, and its running jobs still count.
    std::deque<LaneSlot> laneSlots;

    // Between taking a job and having destroyed it, so that what its captures
    // do in their destructors counts as well.
    class LaneBusy {
    public:
      explicit LaneBusy(LaneSlot &slot) noexcept : m_slot(slot) {}
      LaneBusy(const LaneBusy &) = delete;
      LaneBusy &operator=(const LaneBusy &) = delete;
      ~LaneBusy() { m_slot.since.store(LANE_IDLE, std::memory_order_release); }

      void enter() noexcept {
        m_slot.since.store(laneEpoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
        // With the fence of backgroundPassed: either it sees this job, or the
        // job sees the value that was put in place before the mark.
        std::atomic_thread_fence(std::memory_order_seq_cst);
      }

    private:
      LaneSlot &m_slot;
    };

    // Lower than the pool, so that a task that computes gets a core only when
    // the workers leave it. Lowering needs no rights; a failure leaves the
    // thread at the normal priority.
    //
    // On Linux SCHED_IDLE, not a niceness: 8 tasks that compute 200 ms
    // against frames of a parallelFor of 1600 x 10 us on all 16 threads
    // (clang, 15 workers, work counted, not timed), median ms per frame: no
    // tasks 1.13-1.18, on the pool 2.44-2.55, on threads of nice 0, 10 and 19
    // alike 2.50-2.71, SCHED_IDLE 1.41-1.49. The tasks take 255 ms then, 544-
    // 624 ms against frames back to back, 241 ms against one frame per 16 ms.
    // Why a niceness does not help was not looked into.
    //
    // A thread of the lane that holds a lock a worker waits for runs only
    // where a core is left, see laneQueue.
    void lowerPriority() {
#ifdef _WIN32
      // Not measured: Windows runs a ready thread of higher priority first.
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elifdef __linux__
      const sched_param param{};
      (void) pthread_setschedparam(pthread_self(), SCHED_IDLE, &param);
#endif
    }

    void runJob(QueuedJob &next, const std::stop_token &stopThread) {
      // Cancelled before it started: dropped, its promise breaks.
      if (next.stop.stop_requested()) {
        return;
      }
      // shutdownBackground stops the thread, and with it the job.
      const std::stop_callback forward(stopThread, [&stop = next.stop] { stop.request_stop(); });
      try {
        next.job(next.stop.get_token());
      } catch (...) {
        // The wrapper of spawn catches everything itself.
        internal::logDetachedException();
      }
    }

    void laneThread(const std::stop_token &stopThread, size_t index, LaneSlot &slot) {
      internal::backgroundThread = true;
      internal::setThreadName("sc-bg-" + std::to_string(index));
      lowerPriority();
      // Whether this thread was woken up for a job it has not found yet, see
      // takeWakeUp of the pool.
      bool woken = false;
      while (!stopThread.stop_requested()) {
        LaneBusy busy(slot);
        QueuedJob next;
        if (!laneQueue.tryPop(next, static_cast<int32_t>(index))) {
          woken = laneSemaphore.try_acquire_for(std::chrono::milliseconds(10));
          continue;
        }
        if (woken) {
          woken = false;
        } else {
          // Nothing to take back if a woken thread was faster.
          (void) laneSemaphore.try_acquire();
        }
        // Taken after shutdownBackground asked: dropped like the ones it
        // finds in the queue.
        if (stopThread.stop_requested()) {
          break;
        }
        busy.enter();
        runJob(next, stopThread);
      }
      slot.taken.store(false, std::memory_order_release);
    }

    // Under laneThreadMutex: a slot no thread has, also none that is about to
    // end.
    LaneSlot &takeLaneSlot() {
      for (LaneSlot &slot: laneSlots) {
        if (!slot.taken.load(std::memory_order_acquire)) {
          slot.taken.store(true, std::memory_order_relaxed);
          return slot;
        }
      }
      LaneSlot &slot = laneSlots.emplace_back();
      slot.taken.store(true, std::memory_order_relaxed);
      return slot;
    }
  } // namespace

  uint32_t defaultBackgroundCount() noexcept { return std::max(std::thread::hardware_concurrency() / 4, 2U); }

  void initBackground(uint32_t threads) {
    std::scoped_lock lock(laneThreadMutex);
    if (!laneThreads.empty()) [[unlikely]] {
      return;
    }

    // Before the handler: a task that logs at exit finds the state still
    // there, see initLogState.
    initLogState();
    static std::once_flag atexitRegistered;
    std::call_once(atexitRegistered, [] { std::atexit(shutdownBackground); });

    laneThreads.reserve(threads);
    for (size_t i = 0; i < threads; ++i) {
      laneThreads.emplace_back(laneThread, i, std::ref(takeLaneSlot()));
    }
    laneThreadCount.store(laneThreads.size(), std::memory_order_release);
  }

  void shutdownBackground() {
    std::vector<std::jthread> stopping;
    {
      std::scoped_lock lock(laneThreadMutex);
      stopping.swap(laneThreads);
      laneThreadCount.store(0, std::memory_order_release);
    }
    // Stops the jobs that are running as well, see runJob.
    for (std::jthread &thread: stopping) {
      thread.request_stop();
    }
    laneSemaphore.release(static_cast<std::ptrdiff_t>(stopping.size()));
    // Joins every thread.
    stopping.clear();

    // What nobody runs any more is dropped, and its wake-ups with it.
    QueuedJob dropped;
    while (laneQueue.tryPop(dropped, 0)) {
      dropped = {};
    }
    while (laneSemaphore.try_acquire()) {
    }
  }

  size_t backgroundThreadCount() noexcept { return laneThreadCount.load(std::memory_order_acquire); }

  void waitUntilBackgroundFinished() {
    for (size_t now = outstanding.load(std::memory_order_acquire); now != 0;
         now = outstanding.load(std::memory_order_acquire)) {
      outstanding.wait(now, std::memory_order_acquire);
    }
  }

  bool onBackgroundThread() noexcept { return internal::backgroundThread; }

  uint64_t markBackground() noexcept { return laneEpoch.fetch_add(1, std::memory_order_relaxed) + 1; }

  bool backgroundPassed(uint64_t mark) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const std::scoped_lock lock(laneThreadMutex);
    return std::ranges::all_of(laneSlots, [mark](const LaneSlot &slot) {
      // Acquire: what a job that ended did with the old value is over.
      return slot.since.load(std::memory_order_acquire) >= mark;
    });
  }

  namespace internal {
    void submitBackground(BackgroundJob job, std::stop_source stop) {
      // Before the push: a thread of the lane may run and finish it right away.
      outstanding.fetch_add(1, std::memory_order_relaxed);
      laneQueue.push({.counted = Outstanding(true), .job = std::move(job), .stop = std::move(stop)});
      laneSemaphore.release();
    }

    std::jthread startService(std::string name, BackgroundJob job) {
      return std::jthread([name = std::move(name), job = std::move(job)](const std::stop_token &stop) mutable {
        backgroundThread = true;
        setThreadName(name);
        try {
          job(stop);
        } catch (...) {
          logDetachedException();
        }
      });
    }
  } // namespace internal

} // namespace sc::threading
