module;
#include "entt/entt.hpp"
export module sc.ecs:entity;


namespace sc::ecs {
  class ChaosRegistry;
  export class ChaosEntity {
    friend class ChaosRegistry;

    template<typename, typename, typename, typename>
    friend class entt::basic_storage;

  public:
    ChaosEntity(entt::entity entity, ChaosRegistry *registry);

    ChaosEntity() = default;

    ChaosEntity(const ChaosEntity &) = default;

    ChaosEntity &operator=(const ChaosEntity &) = default;

    operator bool() const { return entity != entt::null && registry != nullptr; }

    operator entt::entity() const { return entity; }

    template<typename T, typename... Args>
    decltype(auto) add(Args &&...args);

    template<typename T>
    decltype(auto) get() const;

  private:
    ChaosEntity(entt::entity entity);

    entt::entity entity{entt::null};
    ChaosRegistry *registry{nullptr};
  };

  export template<typename T>
  concept IsChaosEntity = std::is_same_v<T, ChaosEntity>;


} // namespace sc::ecs
