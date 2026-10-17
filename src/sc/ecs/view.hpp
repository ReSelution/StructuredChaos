#pragma once

#include <entt/entity/fwd.hpp>
#include <entt/entity/view.hpp>

namespace sc::ecs {

  // Names components the entities of a View must not have.
  template<typename... Components>
  struct Exclude {};

} // namespace sc::ecs

namespace sc::ecs::internal {

  // The EnTT view for the parameters of View, which are sorted into the
  // components to get and the excluded ones.
  template<typename Get, typename Excluded, typename... Components>
  struct ViewOf;

  template<typename... Get, typename... Excluded>
  struct ViewOf<entt::get_t<Get...>, entt::exclude_t<Excluded...>> {
    using type =
        entt::basic_view<entt::get_t<entt::storage_for_t<Get>...>, entt::exclude_t<entt::storage_for_t<Excluded>...>>;
  };

  template<typename... Get, typename... Excluded, typename Component, typename... Rest>
  struct ViewOf<entt::get_t<Get...>, entt::exclude_t<Excluded...>, Component, Rest...>
      : ViewOf<entt::get_t<Get..., Component>, entt::exclude_t<Excluded...>, Rest...> {};

  template<typename... Get, typename... Excluded, typename... More, typename... Rest>
  struct ViewOf<entt::get_t<Get...>, entt::exclude_t<Excluded...>, Exclude<More...>, Rest...>
      : ViewOf<entt::get_t<Get...>, entt::exclude_t<Excluded..., More...>, Rest...> {};

} // namespace sc::ecs::internal

namespace sc::ecs {

  // An EnTT view over the given components. A const component is only read:
  // View<Position, const Velocity> writes Position and reads Velocity. As a
  // parameter of a system, this is what tells the scheduler which systems may
  // run at the same time.
  //
  // Exclude leaves out the entities that have one of its components:
  // View<Position, Exclude<Frozen>>.
  template<typename... Components>
  using View = internal::ViewOf<entt::get_t<>, entt::exclude_t<>, Components...>::type;

} // namespace sc::ecs
