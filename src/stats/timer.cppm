
module;

#include <chrono>
#include <ratio>
#include <string_view> // Explizit für std::string_view
#include <type_traits>
#include <utility>

export module sc.stats:timer;

namespace sc::stats {
  export enum class Unit { Auto, Nano, Micro, Milli, Seconds };

  export struct TimeResult {
    double value;
    Unit unit;
    std::string_view suffix;
  };

  export template<typename Func>
  class Timer {
  public:
    [[nodiscard]] explicit Timer(Func &&callback, const Unit unit = Unit::Auto) :
        m_unit(unit), m_callback(std::forward<Func>(callback)), m_start(std::chrono::steady_clock::now()) {}

    // Move-Konstruktor
    Timer(Timer &&other) noexcept :
        m_unit(other.m_unit), m_callback(std::move(other.m_callback)), m_start(other.m_start),
        m_stopped(other.m_stopped) {
      other.m_stopped = true;
    }

    // Move-Assignment
    Timer &operator=(Timer &&other) noexcept {
      if (this != &other) {
        stop();
        m_unit          = other.m_unit;
        m_callback      = std::move(other.m_callback);
        m_start         = other.m_start;
        m_stopped       = other.m_stopped;
        other.m_stopped = true;
      }
      return *this;
    }

    // Kopieren verbieten (Verhindert doppelte Callbacks)
    Timer(const Timer &)            = delete;
    Timer &operator=(const Timer &) = delete;

    ~Timer() { stop(); }

    void stop() {
      if (m_stopped)
        return;

      // C++26 erlaubt hier oft eine noch präzisere Auflösung, steady_clock ist
      // meist sicherer als high_resolution_clock
      auto end  = std::chrono::steady_clock::now();
      auto diff = end - m_start;

      m_callback(calculate(diff));
      m_stopped = true;
    }

  private:
    [[nodiscard]] TimeResult calculate(std::chrono::steady_clock::duration diff) const {
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
    std::chrono::time_point<std::chrono::steady_clock> m_start; // steady_clock empfohlen
    bool m_stopped = false;
  };

  // --- C++17/20/26 Deduction Guide ---
  // Erlaubt es, die 'make_timer'-Helfer komplett wegzulassen!
  export template<typename Func>
  Timer(Func &&, Unit = Unit::Auto) -> Timer<std::decay_t<Func>>;

} // namespace sc::stats
