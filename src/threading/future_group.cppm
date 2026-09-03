
module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <future>
#include <utility>
#include <vector>

export module sc.threading:future_group;

namespace sc::threading {

  /// @brief Aggregates multiple std::future instances into a single synchronizable unit.
  /// @tparam T The return type of the underlying asynchronous tasks.
  template<typename T>
  class [[nodiscard]] FutureGroup {
  public:
    FutureGroup() = default;

    /// @brief Constructs a FutureGroup from an existing vector of futures.
    explicit FutureGroup(std::vector<std::future<T>> futures) : futures_(std::move(futures)) {}

    // Move-only interface (std::future cannot be copied)
    FutureGroup(FutureGroup &&) noexcept            = default;
    FutureGroup &operator=(FutureGroup &&) noexcept = default;
    FutureGroup(const FutureGroup &)                = delete;
    FutureGroup &operator=(const FutureGroup &)     = delete;

    void push(std::future<T> f) { futures_.push_back(std::move(f)); }

    void reserve(size_t capacity) { futures_.reserve(capacity); }

    void wait() const {
      for (const auto &f: futures_) {
        if (f.valid()) {
          f.wait();
        }
      }
    }


    [[nodiscard]] bool ready() const {
      return std::ranges::all_of(futures_, [](const auto &f) {
        return !f.valid() || f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
      });
    }


    [[nodiscard]] size_t completed_count() const {
      return std::ranges::count_if(futures_, [](const auto &f) {
        return !f.valid() || f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
      });
    }


    [[nodiscard]] float progress() const noexcept {
      if (futures_.empty()) {
        return 1.0f;
      }
      return static_cast<float>(completed_count()) / static_cast<float>(futures_.size());
    }


    auto get() {
      if constexpr (std::is_void_v<T>) {
        for (auto &f: futures_) {
          if (f.valid()) {
            f.get();
          }
        }
      } else {
        std::vector<T> results;
        results.reserve(futures_.size());
        for (auto &f: futures_) {
          if (f.valid()) {
            results.push_back(f.get());
          }
        }
        return results;
      }
    }

    [[nodiscard]] size_t size() const noexcept { return futures_.size(); }

    [[nodiscard]] size_t valid_count() const {
      return std::ranges::count_if(futures_, [](const auto &f) { return f.valid(); });
    }

    [[nodiscard]] bool empty() const noexcept { return futures_.empty(); }

    void clear() noexcept { futures_.clear(); }

  private:
    std::vector<std::future<T>> futures_;
  };

} // namespace sc::threading
