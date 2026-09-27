#pragma once

#include <entt/entity/storage.hpp>

namespace sc::ecs {

class Registry;

class Entity {
  friend class Registry;

  template <typename, typename, typename> friend class entt::basic_storage;

public:
  Entity(entt::entity entity, Registry *registry);

  Entity() = default;

  Entity(const Entity &) = default;

  Entity &operator=(const Entity &) = default;

  explicit operator bool() const {
    return entity != entt::null && registry != nullptr;
  }

  operator entt::entity() const { return entity; }

  template <typename T, typename... Args> decltype(auto) add(Args &&...args);

  template <typename T> decltype(auto) get() const;

  entt::entity entity{entt::null};
  Registry *registry{nullptr};

private:
  Entity(entt::entity entity);
};

template <typename T>
concept IsEntity = std::is_same_v<T, Entity>;

} // namespace sc::ecs
