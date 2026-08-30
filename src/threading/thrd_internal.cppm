module;
#include <atomic>
#include <cstddef>
#include <functional>
#include <future>
#include <utility>
export module sc.threading:internal;


export namespace sc::threading {

  using MoveOnlyFunction       = std::move_only_function<void(int)>;
  constexpr uint64_t QUEUE_CAP = 1000000;
  constexpr size_t SFO_LIMIT   = 64;

  struct BatchState {
    std::atomic<size_t> remaining;
    std::promise<void> batch_promise;
    std::atomic_flag exception_set = ATOMIC_FLAG_INIT;
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
  struct DetachedMergedStateNoCallback {
    std::decay_t<R> payload;
    std::tuple<std::decay_t<Args>...> saved_args;
    DetachedMergedStateNoCallback(R &&r, Args &&...a) :
        payload(std::forward<R>(r)), saved_args(std::forward<Args>(a)...) {}
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


} // namespace sc::threading
