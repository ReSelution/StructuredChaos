#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

#include <array>
#include <atomic>
#include <span>
#include <vector>

#include "glm/glm.hpp"
#include "sc/ecs/entity.hpp"
#include "sc/ecs/registry.hpp"
#include "sc/ecs/resource.hpp"
#include "sc/stats/core.hpp"
#include "sc/threading/threading.hpp"

namespace {

struct Position {
  float x{0.0f};
  float y{0.0f};
};

struct Velocity {
  float dx{0.0f};
  float dy{0.0f};
};

struct TransformComponent {
  glm::mat4 transform{1.0f};
};

struct PhysicsComponent {
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
  glm::vec3 velocity{0.0f};
  float mass{1.0f};
};

struct TagComponent {
  std::array<char, 32> name{};
};

struct AIStateComponent {
  int currentState{0};
  glm::vec3 targetPos{0.0f};
  float stamina{100.0f};
};

struct MeshComponent {
  sc::ecs::Resource<std::span<const float>> vertices;
  uint32_t meshId{0};
};

struct MockResource {
  bool cleared{false};
  void clear() { cleared = true; }
};

struct ComplexComponent {
  int id{0};
  MockResource res{};
};

struct MoveCopySpy {
  static inline std::atomic<size_t> copyCount{0};
  static inline std::atomic<size_t> moveCount{0};
  static inline std::atomic<size_t> ctorCount{0};

  float data[4]{0.0f};

  MoveCopySpy() { ctorCount++; }

  MoveCopySpy(const MoveCopySpy &other) {
    copyCount++;
    for (int i = 0; i < 4; ++i)
      data[i] = other.data[i];
  }

  MoveCopySpy(MoveCopySpy &&other) noexcept {
    moveCount++;
    for (int i = 0; i < 4; ++i)
      data[i] = other.data[i];
  }

  static void reset() {
    copyCount = 0;
    moveCount = 0;
    ctorCount = 0;
  }
};

} // namespace

TEST_CASE("Registry Basic Entity and Component Operations", "[ecs][registry]") {
  sc::ecs::Registry registry;

  SECTION("Single Entity Creation and Emplace") {
    auto e = registry.create();

    registry.emplace<Position>(e, 10.0f, 20.0f);

    auto [lock, pos] = registry.get<Position>(e);
    REQUIRE(pos.x == 10.0f);
    REQUIRE(pos.y == 20.0f);
  }

  SECTION("Multiple Components on Single Entity") {
    auto e = registry.create();
    const auto ent_id = static_cast<entt::entity>(e);

    registry.emplace<Position>(ent_id, 1.0f, 2.0f);
    registry.emplace<Velocity>(ent_id, 0.5f, -0.5f);

    auto [lock, pos, vel] = registry.get<Position, Velocity>(ent_id);
    REQUIRE(pos.x == 1.0f);
    REQUIRE(vel.dx == 0.5f);
  }

  SECTION("Erase Component") {
    auto e = registry.create();
    const auto ent_id = static_cast<entt::entity>(e);

    registry.emplace<Position>(ent_id, 5.0f, 5.0f);
    registry.erase<Position>(ent_id);
  }
}

TEST_CASE("Registry Bulk Entity Creation", "[ecs][registry]") {
  sc::ecs::Registry registry;
  constexpr size_t count = 100;
  std::vector<sc::ecs::Entity> entities(count);

  registry.create(entities.begin(), entities.end());

  REQUIRE(entities.size() == count);

  for (size_t i = 0; i < count; ++i) {
    registry.emplace<Position>(static_cast<entt::entity>(entities[i]),
                               static_cast<float>(i), 0.0f);
  }

  auto [lock, pos] =
      registry.get<Position>(static_cast<entt::entity>(entities[42]));
  REQUIRE(pos.x == 42.0f);
}

TEST_CASE("Registry PFR Resource Cleanup on Destroy", "[ecs][registry][pfr]") {
  sc::ecs::Registry registry;
  auto e = registry.create();
  const auto ent_id = static_cast<entt::entity>(e);

  registry.emplace<ComplexComponent>(ent_id, 1337, MockResource{false});

  SECTION("Cleanup via erase") { registry.erase<ComplexComponent>(ent_id); }
}

TEST_CASE("Registry Multi-Threaded Stress Test", "[ecs][registry][threading]") {
  sc::threading::init();

  constexpr size_t total_entities = 10000;
  constexpr size_t batch_size = 1000;

  sc::ecs::Registry registry;
  registry.reserve<TransformComponent>(total_entities);
  registry.reserve<PhysicsComponent>(total_entities);
  registry.reserve<TagComponent>(total_entities);
  registry.reserve<MeshComponent>(total_entities);

  std::vector<int> entitiesStart;
  for (size_t i = 0; i < total_entities; i += batch_size) {
    entitiesStart.emplace_back(static_cast<int>(i));
  }

  sc::threading::detachBatch(
      std::move(entitiesStart),
      [&registry](int thread_id, int start) {
        float rawData[200]{};
        for (size_t i = 0; i < batch_size; ++i) {
          auto e = registry.create();
          e.add<TransformComponent>();
          e.add<PhysicsComponent>();
          e.add<TagComponent>();
          e.add<AIStateComponent>();
          e.add<MeshComponent>(std::span<const float>(rawData, 200),
                               static_cast<uint32_t>(start + i));
        }
      },
      nullptr);

  sc::threading::wait_until_finished();

  auto view = registry.view<PhysicsComponent>();
  REQUIRE(view.size() == total_entities);
}

TEST_CASE("Registry Range Insert and Move/Copy Behavior",
          "[ecs][registry][semantics]") {
  constexpr size_t batch_size = 100;

  SECTION("Copy Insert") {
    sc::ecs::Registry registry;
    MoveCopySpy::reset();

    std::vector<MoveCopySpy> spies{batch_size};
    std::vector<sc::ecs::Entity> entities{batch_size};
    registry.create(entities.begin(), entities.end());

    registry.insert<MoveCopySpy>(entities.begin(), entities.end(),
                                 spies.begin());

    REQUIRE(MoveCopySpy::copyCount.load() >= batch_size);
  }

  SECTION("Move Insert") {
    sc::ecs::Registry registry;
    MoveCopySpy::reset();

    std::vector<MoveCopySpy> spies{batch_size};
    std::vector<sc::ecs::Entity> entities{batch_size};
    registry.create(entities.begin(), entities.end());

    registry.insert<MoveCopySpy>(entities.begin(), entities.end(),
                                 std::make_move_iterator(spies.begin()));

    REQUIRE(MoveCopySpy::moveCount.load() >= batch_size);
    REQUIRE(MoveCopySpy::copyCount.load() == 0);
  }
}
