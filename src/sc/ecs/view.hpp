#pragma once

#include <utility>

namespace sc::ecs {

  template <typename Lock, typename EnTTView> class View {
    Lock m_lock;
    EnTTView m_view;

  public:
    View(Lock &&lock, EnTTView view) : m_lock(std::move(lock)), m_view(std::move(view)) {}

    View(std::tuple<Lock, EnTTView> &&tuple) : View(std::move(std::get<0>(tuple)), std::move(std::get<1>(tuple))) {}

    [[nodiscard]] auto begin() const { return m_view.begin(); }
    [[nodiscard]] auto end() const { return m_view.end(); }
    auto begin() { return m_view.begin(); }
    auto end() { return m_view.end(); }

    template <typename... Args> decltype(auto) get(Args &&...args) const {
      return m_view.get(std::forward<Args>(args)...);
    }

    template <typename... Args> decltype(auto) get(Args &&...args) { return m_view.get(std::forward<Args>(args)...); }

    [[nodiscard]] auto size() const noexcept { return m_view.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_view.empty(); }

    [[nodiscard]] const EnTTView &raw() const noexcept { return m_view; }
    EnTTView &raw() noexcept { return m_view; }
  };

  template <typename Lock, typename EnTTView> View(std::tuple<Lock, EnTTView> &&) -> View<Lock, EnTTView>;

} // namespace sc::ecs
