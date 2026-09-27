#include "entity.hpp"

namespace sc::ecs {
Entity::Entity(entt::entity entity, Registry *registry)
    : entity(entity), registry(registry) {}

Entity::Entity(entt::entity entity) : entity(entity), registry(nullptr) {}
} // namespace sc::ecs
