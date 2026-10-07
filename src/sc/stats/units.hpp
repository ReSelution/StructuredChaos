#pragma once

#include <array>
#include <cmath>
#include <string>
#include <string_view>

#include <format>

namespace sc::stats {

struct DataUnits {
  static constexpr double base = 1024.0;
  static constexpr std::array suffixes{"B", "KB", "MB", "GB", "TB", "PB"};
};

struct MetricUnits {
  static constexpr double base = 1000.0;
  static constexpr std::array suffixes{"", "K", "M", "B",
                                       "T"}; // for counts (1K, 1M...)
};

template <typename System> struct ChaosFormatter {
  [[nodiscard]] static std::string format(double value,
                                          std::string_view time_suffix = "") {
    size_t i = 0;
    double v = value;

    // Compared as it will be printed: 999.999 shows as "1000.00" and belongs
    // to the next unit already.
    const auto printed = [](double x) {
      return std::round(std::abs(x) * 100.0) / 100.0;
    };
    while (printed(v) >= System::base && i < System::suffixes.size() - 1) {
      v /= System::base;
      i++;
    }

    const std::string_view suffix = System::suffixes[i];
    if (suffix.empty() && time_suffix.empty()) {
      // No unit, so no space after the number either.
      return std::format("{:.2f}", v);
    }
    return std::format("{:.2f} {}{}", v, suffix, time_suffix);
  }
};

} // namespace sc::stats
