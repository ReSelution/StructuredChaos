//
// Created by oleub on 03.03.26.
//

#pragma once
#include <chrono>

namespace SC {
  enum class Unit { Auto, Nano, Micro, Milli, Seconds };

  struct TimeResult {
    double value;
    Unit unit;
    std::string_view suffix;
  };

  template<typename Func>
  class ChaosTimer {
  public:
    explicit ChaosTimer(Func &&callback, const Unit unit = Unit::Auto) : m_unit(unit),
                                                                         m_callback(std::forward<Func>(callback)),
                                                                         m_start(
                                                                           std::chrono::high_resolution_clock::now()) {
    }

    ChaosTimer(ChaosTimer &&other) noexcept
      : m_unit(other.m_unit),
        m_callback(std::move(other.m_callback)),
        m_start(other.m_start),
        m_stopped(other.m_stopped) {
      other.m_stopped = true;
    }

    ChaosTimer &operator=(ChaosTimer &&other) noexcept {
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

    ~ChaosTimer() { stop(); }

    void stop() {
      if (m_stopped) return;
      auto end = std::chrono::high_resolution_clock::now();
      auto diff = end - m_start;

      TimeResult res = calculate(diff);
      m_callback(res);
      m_stopped = true;
    }

  private:
    [[nodiscard]] TimeResult calculate(std::chrono::high_resolution_clock::duration diff) const {
      switch (m_unit) {
        case Unit::Nano: return {std::chrono::duration<double, std::nano>(diff).count(), m_unit, "ns"};
        case Unit::Micro: return {std::chrono::duration<double, std::micro>(diff).count(), m_unit, "µs"};
        case Unit::Milli: return {std::chrono::duration<double, std::milli>(diff).count(), m_unit, "ms"};
        case Unit::Seconds: return {std::chrono::duration<double>(diff).count(), m_unit, "s"};
        default: {
          if (diff < std::chrono::microseconds(1))
            return {std::chrono::duration<double, std::nano>(diff).count(), Unit::Nano, "ns"};
          if (diff < std::chrono::milliseconds(1))
            return {std::chrono::duration<double, std::micro>(diff).count(), Unit::Micro, "µs"};
          if (diff < std::chrono::seconds(1))
            return {std::chrono::duration<double, std::milli>(diff).count(), Unit::Milli, "ms"};
          return {std::chrono::duration<double>(diff).count(), Unit::Seconds, "s"};
        }
      }
    }

    Unit m_unit;
    Func m_callback;
    std::chrono::time_point<std::chrono::high_resolution_clock> m_start;
    bool m_stopped = false;
  };

  template<typename Func>
  [[nodiscard]] static auto make_timer(Func &&cb, Unit unit) { return ChaosTimer<Func>(std::forward<Func>(cb), unit); }

  template<typename Func>
  [[nodiscard]] static auto make_timer(Func &&cb) { return ChaosTimer<Func>(std::forward<Func>(cb)); }
} // SC
