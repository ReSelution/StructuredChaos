
module;

#include <array>
#include <cmath> // Für std::abs
#include <format>
#include <string>
#include <string_view>

export module SC.Stats:Units;

namespace SC {

export struct DataUnits {
  static constexpr double base = 1024.0;
  static constexpr std::array suffixes{"B", "KB", "MB", "GB", "TB", "PB"};
};

export struct MetricUnits {
  static constexpr double base = 1000.0;
  static constexpr std::array suffixes{"", "K", "M", "B",
                                       "T"}; // Für Counts (1K, 1M...)
};

export template <typename System> struct ChaosFormatter {
  [[nodiscard]] static std::string format(double value,
                                          std::string_view time_suffix = "") {
    size_t i = 0;
    double v = value;

    // Nutze std::abs aus <cmath>
    while (std::abs(v) >= System::base && i < System::suffixes.size() - 1) {
      v /= System::base;
      i++;
    }

    return std::format("{:.2f} {}{}", v, System::suffixes[i], time_suffix);
  }
};

} // namespace SC
