#pragma once

#include <entt/entity/fwd.hpp>
#include <entt/entity/group.hpp>

namespace sc::ecs {

  // An EnTT group, which keeps the entities that have all of its components
  // packed together and is faster to iterate than a view:
  //
  //   Group<entt::owned_t<Position, const Velocity>, entt::get_t<const Mass>>
  //
  // Like for a View, a const component is only read.
  template<typename Owned, typename Get = entt::get_t<>, typename Exclude = entt::exclude_t<>>
  using Group = entt::group<Owned, Get, Exclude>;

} // namespace sc::ecs
