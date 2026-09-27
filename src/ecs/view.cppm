module;

#include <utility>
export module sc.ecs:view;

namespace sc::ecs {

  template<typename Lock, typename EnTTView>
  class ChaosView {
    Lock m_lock;
    EnTTView m_view;

  public:
    ChaosView(Lock &&lock, EnTTView view) : m_lock(std::move(lock)), m_view(std::move(view)) {}

    ChaosView(std::tuple<Lock, EnTTView> &&tuple) :
        ChaosView(std::move(std::get<0>(tuple)), std::move(std::get<1>(tuple))) {}

    auto begin() const { return m_view.begin(); }
    auto end() const { return m_view.end(); }
    auto begin() { return m_view.begin(); }
    auto end() { return m_view.end(); }

    template<typename... Args>
    decltype(auto) get(Args &&...args) const {
      return m_view.get(std::forward<Args>(args)...);
    }

    template<typename... Args>
    decltype(auto) get(Args &&...args) {
      return m_view.get(std::forward<Args>(args)...);
    }

    auto size() const noexcept { return m_view.size(); }
    bool empty() const noexcept { return m_view.empty(); }

    const EnTTView &raw() const noexcept { return m_view; }
    EnTTView &raw() noexcept { return m_view; }
  };

  template<typename Lock, typename EnTTView>
  ChaosView(std::tuple<Lock, EnTTView> &&) -> ChaosView<Lock, EnTTView>;

} // namespace sc::ecs
