#pragma once



#include <chrono>
#include <concepts>
#include <ratio>
#include <string_view>
#include <type_traits>
#include <utility>

namespace sc::stats {
enum class Unit { Auto, Nano, Micro, Milli, Seconds };

struct TimeResult {
  double value;
  Unit unit;
  std::string_view suffix;
};

template <typename Func> class Timer {
public:
  // Takes the callback by value or by reference: Func is the decayed type, so
  // a parameter of type Func&& would only accept temporaries.
  template <typename F>
    requires std::constructible_from<Func, F>
  [[nodiscard]] explicit Timer(F &&callback, const Unit unit = Unit::Auto)
      : m_unit(unit), m_callback(std::forward<F>(callback)),
        m_start(std::chrono::steady_clock::now()) {}

  Timer(Timer &&other) noexcept
      : m_unit(other.m_unit), m_callback(std::move(other.m_callback)),
        m_start(other.m_start), m_stopped(other.m_stopped) {
    other.m_stopped = true;
  }

  Timer &operator=(Timer &&other) noexcept {
    if (this != &other) {
      stop();
      m_unit = other.m_unit;
      m_callback = std::move(other.m_callback);
      m_start = other.m_start;
      m_stopped = other.m_stopped;
      other.m_stopped = true;
    }
    return *this;
  }

  // No copies: the callback would run twice.
  Timer(const Timer &) = delete;
  Timer &operator=(const Timer &) = delete;

  ~Timer() { stop(); }

  void stop() {
    if (m_stopped)
      return;

    auto end = std::chrono::steady_clock::now();
    auto diff = end - m_start;

    m_callback(calculate(diff));
    m_stopped = true;
  }

private:
  [[nodiscard]] TimeResult
  calculate(std::chrono::steady_clock::duration diff) const {
    using namespace std::chrono;

    switch (m_unit) {
    case Unit::Nano:
      return {duration<double, std::nano>(diff).count(), m_unit, "ns"};
    case Unit::Micro:
      return {duration<double, std::micro>(diff).count(), m_unit, "µs"};
    case Unit::Milli:
      return {duration<double, std::milli>(diff).count(), m_unit, "ms"};
    case Unit::Seconds:
      return {duration<double>(diff).count(), m_unit, "s"};
    default: {
      if (diff < microseconds(1))
        return {duration<double, std::nano>(diff).count(), Unit::Nano, "ns"};
      if (diff < milliseconds(1))
        return {duration<double, std::micro>(diff).count(), Unit::Micro, "µs"};
      if (diff < seconds(1))
        return {duration<double, std::milli>(diff).count(), Unit::Milli, "ms"};
      return {duration<double>(diff).count(), Unit::Seconds, "s"};
    }
    }
  }

  Unit m_unit;
  Func m_callback;
  std::chrono::time_point<std::chrono::steady_clock> m_start;
  bool m_stopped = false;
};

// Lets Timer(callback) work without naming the type of the callback.
template <typename Func>
Timer(Func &&, Unit = Unit::Auto) -> Timer<std::decay_t<Func>>;

} // namespace sc::stats
