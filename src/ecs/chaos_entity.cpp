//
// Created by oleub on 26.03.26.
//

#include "chaos_registry.hpp"

namespace SC {
  ChaosEntity::ChaosEntity(entt::entity entity, ChaosRegistry *registry): entity(entity), registry(registry) {

  }

  ChaosEntity::ChaosEntity(entt::entity entity):entity(entity), registry(nullptr) {

  }
} // SC