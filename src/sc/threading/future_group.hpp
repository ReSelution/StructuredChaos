#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <type_traits>
#include <utility>
#include <vector>

namespace sc::threading {

  // Several futures of the same type, waited for and collected together.
  template<typename T>
  class [[nodiscard]] FutureGroup {
  public:
    FutureGroup() = default;

    explicit FutureGroup(std::vector<std::future<T>> futures) : m_futures(std::move(futures)) {}

    // Move-only, like std::future.
    FutureGroup(FutureGroup &&) noexcept = default;
    FutureGroup &operator=(FutureGroup &&) noexcept = default;
    FutureGroup(const FutureGroup &) = delete;
    FutureGroup &operator=(const FutureGroup &) = delete;

    void push(std::future<T> f) { m_futures.push_back(std::move(f)); }

    void reserve(size_t capacity) { m_futures.reserve(capacity); }

    void wait() const {
      for (const auto &f: m_futures) {
        if (f.valid()) {
          f.wait();
        }
      }
    }

    [[nodiscard]] bool ready() const {
      return std::ranges::all_of(m_futures, [](const auto &f) {
        return !f.valid() || f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
      });
    }

    [[nodiscard]] size_t completedCount() const {
      return std::ranges::count_if(m_futures, [](const auto &f) {
        return !f.valid() || f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
      });
    }

    [[nodiscard]] float progress() const {
      if (m_futures.empty()) {
        return 1.0f;
      }
      return static_cast<float>(completedCount()) / static_cast<float>(m_futures.size());
    }

    // Collects every result. All futures are consumed even if a task threw;
    // the first exception in push order is rethrown afterwards.
    auto get() {
      std::exception_ptr firstError;
      auto consume = [&firstError](auto &&take) {
        try {
          take();
        } catch (...) {
          if (!firstError) {
            firstError = std::current_exception();
          }
        }
      };

      if constexpr (std::is_void_v<T>) {
        for (auto &f: m_futures) {
          if (f.valid()) {
            consume([&f] { f.get(); });
          }
        }
        if (firstError) {
          std::rethrow_exception(firstError);
        }
      } else {
        std::vector<T> results;
        results.reserve(m_futures.size());
        for (auto &f: m_futures) {
          if (f.valid()) {
            consume([&f, &results] { results.push_back(f.get()); });
          }
        }
        if (firstError) {
          std::rethrow_exception(firstError);
        }
        return results;
      }
    }

    [[nodiscard]] size_t size() const noexcept { return m_futures.size(); }

    [[nodiscard]] size_t validCount() const {
      return std::ranges::count_if(m_futures, [](const auto &f) { return f.valid(); });
    }

    [[nodiscard]] bool empty() const noexcept { return m_futures.empty(); }

    void clear() noexcept { m_futures.clear(); }

  private:
    std::vector<std::future<T>> m_futures;
  };

} // namespace sc::threading
