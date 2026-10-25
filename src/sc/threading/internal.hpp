#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <tuple>
#include <type_traits>
#include <utility>

namespace sc::threading::internal {

  using MoveOnlyFunction = std::move_only_function<void()>;

  // An element of a range passed as R: moved from if the range is an rvalue,
  // copied otherwise. std::forward_like does the same, but clang cannot use
  // the one of libstdc++ 14 in a lambda capture, as its return type is
  // deduced.
  template<typename R, typename T>
  using ElementRef = std::conditional_t<std::is_lvalue_reference_v<R>, T &, T &&>;

  template<typename R, typename T>
  constexpr ElementRef<R, T> forwardElement(T &item) noexcept {
    return static_cast<ElementRef<R, T>>(item);
  }

  // Capacity of the queue of the pool. The queue blocks a producer while it is
  // full, which could stall a pool whose workers all produce, so it is large.
  // The memory of a slot is only touched when it is used, see RingQueue.
  inline constexpr uint64_t QUEUE_CAP = 1000000;

  // Batches whose captures (function, extra arguments and one element) fit in
  // this many bytes copy them into every task. Larger ones keep them once, in a
  // state shared by all tasks of the batch. This is not the small-buffer size
  // of std::move_only_function, which is implementation defined (16 bytes in
  // libstdc++), so a task may allocate either way; it only bounds how much is
  // copied per task.
  inline constexpr size_t INLINE_CAPTURE_LIMIT = 64;

  // The completion of a batch with one future for all of its tasks. The future
  // becomes ready once every task has finished, with the first exception if a
  // task threw.
  struct BatchState {
    std::atomic<size_t> remaining;
    std::promise<void> batchPromise;
    std::atomic_flag failed = ATOMIC_FLAG_INIT;
    std::exception_ptr error;

    explicit BatchState(size_t n) : remaining(n) {}

    // Called by every task when it is done, with the exception it threw or
    // null.
    void finishTask(std::exception_ptr taskError) noexcept {
      if (taskError && !failed.test_and_set(std::memory_order_acq_rel)) {
        error = std::move(taskError);
      }
      if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (error) {
          batchPromise.set_exception(error);
        } else {
          batchPromise.set_value();
        }
      }
    }
  };

  template<typename R, typename... Args>
  struct MergedState : BatchState {
    std::decay_t<R> payload;
    std::tuple<std::decay_t<Args>...> savedArgs;
    MergedState(size_t n, R &&r, Args &&...a) :
        BatchState(n), payload(std::forward<R>(r)), savedArgs(std::forward<Args>(a)...) {}
  };

  struct DetachBatchState {
    std::atomic<size_t> remaining;
    MoveOnlyFunction onFinished;
    DetachBatchState(size_t n, MoveOnlyFunction &&cb) : remaining(n), onFinished(std::move(cb)) {}

    // Called by every task when it is done; the last one calls onFinished.
    void finishTask() {
      if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1 && onFinished) {
        onFinished();
      }
    }
  };

  template<typename R, typename... Args>
  struct DetachedMergedState : DetachBatchState {
    std::decay_t<R> payload;
    std::tuple<std::decay_t<Args>...> savedArgs;
    DetachedMergedState(size_t n, R &&r, MoveOnlyFunction &&cb, Args &&...a) :
        DetachBatchState(n, std::move(cb)), payload(std::forward<R>(r)), savedArgs(std::forward<Args>(a)...) {}
  };

  template<typename R, typename... Args>
  struct DetachedMergedStateNoCallback {
    std::decay_t<R> payload;
    std::tuple<std::decay_t<Args>...> savedArgs;
    explicit DetachedMergedStateNoCallback(R &&r, Args &&...a) :
        payload(std::forward<R>(r)), savedArgs(std::forward<Args>(a)...) {}
  };

} // namespace sc::threading::internal
