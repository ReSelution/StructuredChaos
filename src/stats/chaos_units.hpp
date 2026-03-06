//
// Created by oleub on 04.03.26.
//

#pragma once
#include <array>
#include <format>
#include <string>

namespace SC {
  struct DataUnits {
    static constexpr double base = 1024.0;
    static constexpr std::array suffixes{"B", "KB", "MB", "GB", "TB", "PB"};
  };

  struct MetricUnits {
    static constexpr double base = 1000.0;
    static constexpr std::array suffixes{"", "K", "M", "B", "T"}; // Für Counts (1K, 1M...)
  };

  template<typename System>
  struct ChaosFormatter {
    static std::string format(double value, std::string_view time_suffix = "") {
      size_t i = 0;
      double v = value;
      while (std::abs(v) >= System::base && i < System::suffixes.size() - 1) {
        v /= System::base;
        i++;
      }
      return std::format("{:.2f} {}{}", v, System::suffixes[i], time_suffix);
    }
  };
}
