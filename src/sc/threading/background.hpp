#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

// The background lane: threads beside the pool for work that takes longer
// than a frame, computes over several frames or blocks in a call. The pool
// belongs to the frame, every one of its workers helps with parallelFor; a
// task that keeps one for long makes the frame wait for the rest. The lane
// shares nothing with the pool, no queue, no wake-ups and no tickets, and its
// threads run at a lower priority of the OS, so that they get a core when the
// workers leave it.
namespace sc::threading {

  // A quarter of the hardware threads, at least two.
  [[nodiscard]] uint32_t defaultBackgroundCount() noexcept;

  // Starts the lane with the given number of threads. More than the hardware
  // has is fine for calls that mostly wait. Does nothing if it is running
  // already. init does this as well.
  void initBackground(uint32_t threads = defaultBackgroundCount());

  // Asks every running task to stop through its token and joins the threads.
  // Tasks that have not started are dropped, get of their BackgroundTask then
  // throws std::future_error. A task that blocks in a call and does not look
  // at its token holds this up until the call returns. shutdown does this as
  // well, and it runs on its own at exit.
  void shutdownBackground();

  // How many threads the lane has, 0 before initBackground and after
  // shutdownBackground.
  [[nodiscard]] size_t backgroundThreadCount() noexcept;

  // Returns once every job that was put on the lane has run or was dropped,
  // its captures destroyed. Waits for good if there are jobs and no lane.
  void waitUntilBackgroundFinished();

  // Whether the calling thread is one of the lane or of a Service. A
  // parallelFor of such a thread gets workers of the pool only while the
  // frame has nothing for them: before every chunk they look for a loop or a
  // task of the frame and leave for it, so the frame waits for at most one
  // chunk per worker that is inside. Keep the chunks short through grain.
  // Without a number of helpers it gets half the workers, see parallelFor.
  [[nodiscard]] bool onBackgroundThread() noexcept;

  // For memory that a task of the lane may still read after it was replaced:
  // call markBackground after the new value is in place, and free the old one
  // once backgroundPassed says so. That is once every task running at the mark
  // has ended; a task that starts later sees the new value. Tasks that are
  // still queued count as later, so a task reads through a handle it captured,
  // not through a reference to the value. A Service does not count: it never
  // ends, and nothing would ever be freed next to one.
  [[nodiscard]] uint64_t markBackground() noexcept;
  [[nodiscard]] bool backgroundPassed(uint64_t mark);

  namespace internal {
    using BackgroundJob = std::move_only_function<void(std::stop_token)>;

    // Queues the job; the lane calls it with the token of stop, unless stop
    // was asked before the job started, then it is dropped.
    void submitBackground(BackgroundJob job, std::stop_source stop);

    // A thread of its own for a Service, at the normal priority.
    [[nodiscard]] std::jthread startService(std::string name, BackgroundJob job);

    template<typename F>
    struct BackgroundResult {
      using type = std::invoke_result_t<F &>;
    };

    template<typename F>
      requires std::invocable<F &, std::stop_token>
    struct BackgroundResult<F> {
      using type = std::invoke_result_t<F &, std::stop_token>;
    };

    // Calls the function with the token if it takes one.
    template<typename F>
    decltype(auto) invokeBackground(F &f, std::stop_token token) {
      if constexpr (std::invocable<F &, std::stop_token>) {
        return f(std::move(token));
      } else {
        (void) token;
        return f();
      }
    }
  } // namespace internal

  // The result of a function spawn put on the lane. Move-only, like
  // std::future. Destroying it asks the task to stop and does not wait: a
  // task that has not started then never runs.
  template<typename T>
  class [[nodiscard]] BackgroundTask {
  public:
    BackgroundTask() = default;

    BackgroundTask(std::future<T> future, std::stop_source stop) :
        m_future(std::move(future)), m_stop(std::move(stop)) {}

    BackgroundTask(BackgroundTask &&) noexcept = default;

    // Asks the task this one had to stop.
    BackgroundTask &operator=(BackgroundTask &&other) noexcept {
      if (this != &other) {
        cancel();
        m_future = std::move(other.m_future);
        m_stop = std::move(other.m_stop);
      }
      return *this;
    }

    BackgroundTask(const BackgroundTask &) = delete;
    BackgroundTask &operator=(const BackgroundTask &) = delete;

    ~BackgroundTask() { cancel(); }

    // False for a default constructed one and after get.
    [[nodiscard]] bool valid() const noexcept { return m_future.valid(); }

    // Whether get returns without waiting. False if not valid.
    [[nodiscard]] bool ready() const {
      return m_future.valid() && m_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    void wait() const { m_future.wait(); }

    // The result or the exception of the function; std::future_error if it
    // was dropped, by cancel before it started or by shutdownBackground.
    T get() { return m_future.get(); }

    // Asks the task to stop through its token. One that has not started is
    // dropped; one that is running decides itself what to do with it.
    void cancel() noexcept { m_stop.request_stop(); }

  private:
    std::future<T> m_future;
    std::stop_source m_stop{std::nostopstate};
  };

  // Runs the function on the lane. It may take a std::stop_token, which tells
  // it that cancel or shutdownBackground asked it to stop; check it between
  // pieces of work. The function must not touch a World: the systems may be
  // running.
  template<typename F>
    requires std::invocable<std::decay_t<F> &, std::stop_token> || std::invocable<std::decay_t<F> &>
  auto spawn(F &&f) -> BackgroundTask<typename internal::BackgroundResult<std::decay_t<F>>::type> {
    using Function = std::decay_t<F>;
    using Result = internal::BackgroundResult<Function>::type;
    std::promise<Result> promise;
    std::future<Result> future = promise.get_future();
    std::stop_source stop;
    internal::submitBackground(
        [f = Function(std::forward<F>(f)), p = std::move(promise)](std::stop_token token) mutable {
          try {
            if constexpr (std::is_void_v<Result>) {
              internal::invokeBackground(f, std::move(token));
              p.set_value();
            } else {
              p.set_value(internal::invokeBackground(f, std::move(token)));
            }
          } catch (...) {
            p.set_exception(std::current_exception());
          }
        },
        stop);
    return {std::move(future), std::move(stop)};
  }

  // A thread of its own for a function that runs as long as the program, as
  // a mixer of audio or a loop that receives from the network. Not on the
  // lane, where it would keep a thread for good, and at the normal priority
  // of the OS, which audio needs. The function gets the token that the
  // destructor and requestStop set; an exception it throws is logged.
  // Destroying the Service asks it to stop and joins it.
  class Service {
  public:
    Service() = default;

    // The name is shown in debuggers and profilers, on Linux the first 15
    // characters.
    template<typename F>
      requires std::invocable<std::decay_t<F> &, std::stop_token>
    Service(std::string name, F &&f) :
        m_thread(internal::startService(std::move(name), internal::BackgroundJob(std::forward<F>(f)))) {}

    [[nodiscard]] bool running() const noexcept { return m_thread.joinable(); }

    void requestStop() noexcept { m_thread.request_stop(); }

  private:
    std::jthread m_thread;
  };

} // namespace sc::threading
