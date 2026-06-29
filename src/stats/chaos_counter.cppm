module;

#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

export module SC.Stats:Counter;
import :Units;

namespace SC {

export struct MetricUnits;

export template <typename UnitSystem = MetricUnits> struct ChaosCounter {
  struct alignas(64) Storage {
    const std::string_view name;
    std::atomic<uint64_t> value{0};
  };

  static void start(Storage &) noexcept {}

  static void stop(Storage &) noexcept {}

  static void record(Storage &s, uint64_t v) noexcept {
    s.value.fetch_add(v, std::memory_order_relaxed);
  }

  static void reset(Storage &s) noexcept {
    s.value.store(0, std::memory_order_relaxed);
  }

  [[nodiscard]] static std::string format(const Storage &s) {
    uint64_t raw_val = s.value.load(std::memory_order_relaxed);
    return std::format(
        "{}", ChaosFormatter<UnitSystem>::format(static_cast<double>(raw_val)));
  }
};

} // namespace SC
