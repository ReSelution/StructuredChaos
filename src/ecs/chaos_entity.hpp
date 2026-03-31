//
// Created by oleub on 26.03.26.
//

#pragma once
#include <entt/entity/entity.hpp>

#include "chaos_registry.hpp"

namespace SC {
  class ChaosEntity {
  public:
    ChaosEntity(entt::entity entity, ChaosRegistry *registry);

    ChaosEntity() = default;

    ChaosEntity(const ChaosEntity &) = default;
    ChaosEntity &operator=(const ChaosEntity &) = default;

    operator bool() const {
      return  entity != entt::null && registry != nullptr;
    }

    template<typename T, typename... Args>
    decltype(auto) add(Args &&... args) {
      return registry->emplace<T>(entity, std::forward<Args>(args)...);
    }

    template<typename T>
    decltype(auto) get() const {
      return registry->get<T>(entity);
    }

  private:
    entt::entity entity{entt::null};
    ChaosRegistry *registry{nullptr};
  };
} // SC
