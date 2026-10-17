#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

#include "sc/ecs/entity.hpp"
#include "sc/ecs/registry.hpp"
#include "sc/ecs/resource.hpp"
#include "sc/ecs/view.hpp"

namespace {

  struct Position {
    float x{0.0f};
    float y{0.0f};
  };

  struct Velocity {
    float dx{0.0f};
    float dy{0.0f};
  };

  struct Frozen {};

  struct MeshComponent {
    sc::ecs::Resource<std::span<const float>> vertices;
    uint32_t meshId{0};
  };

  std::vector<sc::ecs::Entity> make_entities(sc::ecs::Registry &registry, size_t count) {
    std::vector<sc::ecs::Entity> entities(count);
    registry.create(entities.begin(), entities.end());
    return entities;
  }

} // namespace

TEST_CASE("Registry Basic Entity and Component Operations", "[ecs][registry]") {
  sc::ecs::Registry registry;

  SECTION("Single Entity Creation and Emplace") {
    auto e = registry.create();
    REQUIRE(e);
    REQUIRE(registry.valid(e));

    registry.emplace<Position>(e, 10.0f, 20.0f);

    auto &pos = registry.get<Position>(e);
    REQUIRE(pos.x == 10.0f);
    REQUIRE(pos.y == 20.0f);
  }

  SECTION("Multiple Components on Single Entity") {
    auto e = registry.create();

    e.add<Position>(1.0f, 2.0f);
    e.add<Velocity>(0.5f, -0.5f);

    auto [pos, vel] = registry.get<Position, Velocity>(e);
    REQUIRE(pos.x == 1.0f);
    REQUIRE(vel.dx == 0.5f);

    pos.x = 7.0f;
    REQUIRE(e.get<Position>().x == 7.0f);
  }

  SECTION("Const Access") {
    auto e = registry.create();
    registry.emplace<Position>(e, 3.0f, 4.0f);
    registry.emplace<Velocity>(e, 1.5f, 2.5f);

    STATIC_REQUIRE(std::is_same_v<decltype(registry.get<const Position>(e)), const Position &>);
    STATIC_REQUIRE(std::is_same_v<decltype(registry.get<Position>(e)), Position &>);

    auto [pos, vel] = registry.get<const Position, Velocity>(e);
    STATIC_REQUIRE(std::is_const_v<std::remove_reference_t<decltype(pos)>>);
    STATIC_REQUIRE(!std::is_const_v<std::remove_reference_t<decltype(vel)>>);
    REQUIRE(pos.y == 4.0f);
    REQUIRE(vel.dy == 2.5f);
  }

  SECTION("Erase Component") {
    auto e = registry.create();
    registry.emplace<Position>(e, 5.0f, 5.0f);
    REQUIRE(registry.view<Position>().size() == 1);

    registry.erase<Position>(e);
    REQUIRE(registry.view<Position>().size() == 0);
    REQUIRE(registry.valid(e));
  }

  SECTION("Insert A Range") {
    const auto entities = make_entities(registry, 100);
    std::vector<Position> positions(entities.size());
    for (size_t i = 0; i < positions.size(); ++i) {
      positions[i].x = static_cast<float>(i);
    }

    registry.insert<Position>(entities.begin(), entities.end(), positions.begin());

    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(registry.get<Position>(entities[i]).x == static_cast<float>(i));
    }
  }
}

TEST_CASE("Registry Views", "[ecs][registry][view]") {
  sc::ecs::Registry registry;
  const auto entities = make_entities(registry, 100);
  for (size_t i = 0; i < entities.size(); ++i) {
    registry.emplace<Position>(entities[i], static_cast<float>(i), 0.0f);
    if (i % 2 == 0) {
      registry.emplace<Velocity>(entities[i], 1.0f, 2.0f);
    }
    if (i % 4 == 0) {
      registry.emplace<Frozen>(entities[i]);
    }
  }

  SECTION("A View Is The Type A System Takes") {
    STATIC_REQUIRE(
        std::is_same_v<decltype(registry.view<Position, const Velocity>()), sc::ecs::View<Position, const Velocity>>);
  }

  SECTION("Const Components Are Read Only") {
    auto view = registry.view<Position, const Velocity>();
    size_t visited = 0;
    for (auto [entity, pos, vel]: view.each()) {
      STATIC_REQUIRE(!std::is_const_v<std::remove_reference_t<decltype(pos)>>);
      STATIC_REQUIRE(std::is_const_v<std::remove_reference_t<decltype(vel)>>);
      pos.y = vel.dy;
      ++visited;
    }
    REQUIRE(visited == 50);
    REQUIRE(registry.get<Position>(entities[0]).y == 2.0f);
    REQUIRE(registry.get<Position>(entities[1]).y == 0.0f);
  }

  SECTION("Excluded Components") {
    auto view = registry.view<Position, const Velocity>(entt::exclude<Frozen>);
    size_t visited = 0;
    for (auto entity: view) {
      REQUIRE(static_cast<size_t>(view.get<Position>(entity).x) % 4 == 2);
      ++visited;
    }
    REQUIRE(visited == 25);
  }

  SECTION("A Component Nobody Added Yet") {
    struct Unused {
      int value{0};
    };
    auto view = registry.view<const Unused>();
    REQUIRE(view.begin() == view.end());
  }

  SECTION("Components Added While Iterating Keep References Valid") {
    // What EnTT promises for a single thread; systems that write a component
    // exclusively rely on it.
    const auto more = make_entities(registry, 5000);
    size_t next = 0;
    size_t visited = 0;
    for (auto [entity, pos]: registry.view<Position>().each()) {
      const float before = pos.x;
      for (size_t i = 0; i < 50 && next < more.size(); ++i) {
        registry.emplace<Position>(more[next++], -1.0f, -1.0f);
      }
      REQUIRE(pos.x == before);
      REQUIRE(before >= 0.0f);
      ++visited;
    }
    REQUIRE(visited == entities.size());
    REQUIRE(registry.view<Position>().size() == entities.size() + more.size());
  }
}

TEST_CASE("Registry Destroy", "[ecs][registry][destroy]") {
  sc::ecs::Registry registry;
  auto entities = make_entities(registry, 100);
  for (const auto &e: entities) {
    registry.emplace<Position>(e);
    registry.emplace<Velocity>(e);
  }

  SECTION("Single Entity") {
    REQUIRE(registry.destroy(entities[3]));
    REQUIRE(!registry.valid(entities[3]));
    REQUIRE(!registry.destroy(entities[3]));
    REQUIRE(registry.view<Position>().size() == 99);
    REQUIRE(registry.view<Velocity>().size() == 99);
    REQUIRE(registry.valid(entities[4]));
  }

  SECTION("Range") {
    registry.destroy(entities.begin(), entities.begin() + 40);
    REQUIRE(registry.view<Position>().size() == 60);
    REQUIRE(registry.view<Velocity>().size() == 60);
    REQUIRE(!registry.valid(entities[0]));
    REQUIRE(registry.valid(entities[40]));
  }

  SECTION("Range With Entities Listed Twice Or Destroyed Before") {
    registry.destroy(entities[7]);
    std::vector<sc::ecs::Entity> range;
    for (size_t i = 0; i < 20; ++i) {
      range.push_back(entities[i]);
      range.push_back(entities[i / 2]);
    }
    registry.destroy(range.begin(), range.end());

    REQUIRE(registry.view<Position>().size() == 80);
    REQUIRE(registry.view<Velocity>().size() == 80);
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(registry.valid(entities[i]) == (i >= 20));
    }
  }
}

TEST_CASE("Registry Resource Components", "[ecs][registry][resource]") {
  constexpr size_t count = 2000;
  constexpr size_t vertex_count = 16;

  sc::ecs::Registry registry;
  const auto entities = make_entities(registry, count);

  // No reserve: the storage grows several times and moves the components.
  for (size_t i = 0; i < count; ++i) {
    std::array<float, vertex_count> vertices{};
    vertices.fill(static_cast<float>(i));
    registry.emplace<MeshComponent>(entities[i], std::span<const float>(vertices), static_cast<uint32_t>(i));
  }

  SECTION("Buffers Survive Storage Growth And Erase") {
    // Erasing moves the last component into the freed slot.
    for (size_t i = 0; i < count; i += 2) {
      registry.erase<MeshComponent>(entities[i]);
    }

    for (size_t i = 1; i < count; i += 2) {
      auto &mesh = registry.get<MeshComponent>(entities[i]);
      REQUIRE(mesh.meshId == i);
      REQUIRE(mesh.vertices.size() == vertex_count);
      REQUIRE(mesh.vertices[0] == static_cast<float>(i));
      REQUIRE(mesh.vertices[vertex_count - 1] == static_cast<float>(i));
    }
  }

  SECTION("Buffers Are Released With The Entity") {
    registry.destroy(entities.begin(), entities.end());
    // Resource fields cannot be move-assigned, so EnTT deletes these components
    // in place and the view has no size.
    auto view = registry.view<MeshComponent>();
    REQUIRE(view.begin() == view.end());
  }
}

TEST_CASE("Registry Concurrent Entity Creation", "[ecs][registry][threading]") {
  constexpr size_t thread_count = 8;
  constexpr size_t per_thread = 5000;

  sc::ecs::Registry registry;
  std::vector<std::vector<entt::entity>> created(thread_count);
  {
    std::vector<std::jthread> threads;
    for (size_t t = 0; t < thread_count; ++t) {
      threads.emplace_back([&registry, &mine = created[t]] {
        mine.reserve(per_thread);
        for (size_t i = 0; i < per_thread; ++i) {
          mine.push_back(registry.create());
        }
      });
    }
  }

  std::vector<entt::entity> all;
  for (const auto &mine: created) {
    all.insert(all.end(), mine.begin(), mine.end());
  }
  std::ranges::sort(all);
  REQUIRE(all.size() == thread_count * per_thread);
  REQUIRE(std::ranges::adjacent_find(all) == all.end());
  REQUIRE(std::ranges::all_of(all, [&](entt::entity e) { return registry.valid(e); }));
}

TEST_CASE("Registry Emplace", "[ecs][registry][emplace]") {
  sc::ecs::Registry registry;

  SECTION("Returns The Component It Built") {
    const auto e = registry.create();
    Position &pos = registry.emplace<Position>(e, 1.0f, 2.0f);
    REQUIRE(&pos == &registry.get<Position>(e));
    pos.x = 5.0f;
    REQUIRE(registry.get<Position>(e).x == 5.0f);
    REQUIRE(registry.get<Position>(e).y == 2.0f);
  }

  SECTION("Without Arguments The Component Has Its Defaults") {
    const auto e = registry.create();
    registry.emplace<Position>(e);
    REQUIRE(registry.get<Position>(e).x == 0.0f);
    REQUIRE(registry.get<Position>(e).y == 0.0f);
  }

  SECTION("Only The Entities It Was Called For Have The Component") {
    const auto entities = make_entities(registry, 100);
    for (size_t i = 0; i < entities.size(); ++i) {
      registry.emplace<Position>(entities[i], static_cast<float>(i), 0.0f);
      if (i % 4 == 0) {
        registry.emplace<Velocity>(entities[i], static_cast<float>(i), 0.0f);
      }
    }

    REQUIRE(registry.view<Position>().size() == 100);
    REQUIRE(registry.view<Velocity>().size() == 25);
    size_t both = 0;
    for (auto [entity, pos, vel]: registry.view<const Position, const Velocity>().each()) {
      REQUIRE(pos.x == vel.dx);
      ++both;
    }
    REQUIRE(both == 25);
  }

  SECTION("Components Keep Their Values While The Storage Grows") {
    constexpr size_t count = 20'000;
    const auto entities = make_entities(registry, count);
    // No reserve, and the first reference is held all the way.
    const Position &first = registry.emplace<Position>(entities[0], 0.0f, -1.0f);
    for (size_t i = 1; i < count; ++i) {
      registry.emplace<Position>(entities[i], static_cast<float>(i), -1.0f);
    }

    REQUIRE(&first == &registry.get<Position>(entities[0]));
    for (size_t i = 0; i < count; ++i) {
      REQUIRE(registry.get<Position>(entities[i]).x == static_cast<float>(i));
      REQUIRE(registry.get<Position>(entities[i]).y == -1.0f);
    }
  }

  SECTION("A Component Without Members") {
    const auto entities = make_entities(registry, 10);
    for (size_t i = 0; i < entities.size(); ++i) {
      registry.emplace<Position>(entities[i]);
      if (i % 2 == 0) {
        registry.emplace<Frozen>(entities[i]);
      }
    }

    REQUIRE(registry.view<Frozen>().size() == 5);
    size_t frozen = 0;
    for ([[maybe_unused]] const auto entity: registry.view<const Position, const Frozen>()) {
      ++frozen;
    }
    REQUIRE(frozen == 5);
  }

  SECTION("Again After Erase") {
    const auto e = registry.create();
    registry.emplace<Position>(e, 1.0f, 1.0f);
    registry.erase<Position>(e);
    registry.emplace<Position>(e, 2.0f, 3.0f);

    REQUIRE(registry.view<Position>().size() == 1);
    REQUIRE(registry.get<Position>(e).x == 2.0f);
    REQUIRE(registry.get<Position>(e).y == 3.0f);
  }

  SECTION("An Entity That Takes The Place Of A Destroyed One Starts Without Components") {
    auto entities = make_entities(registry, 1);
    const sc::ecs::Entity old = entities[0];
    registry.emplace<Position>(old, 1.0f, 1.0f);
    registry.emplace<Velocity>(old, 1.0f, 1.0f);
    REQUIRE(registry.destroy(old));

    // A range goes to the registry right away, which hands the number out again.
    registry.create(entities.begin(), entities.end());
    const sc::ecs::Entity next = entities[0];
    REQUIRE(entt::to_entity(next.entity) == entt::to_entity(old.entity));
    REQUIRE(registry.view<Position>().size() == 0);
    REQUIRE(registry.view<Velocity>().size() == 0);

    registry.emplace<Position>(next, 7.0f, 8.0f);
    REQUIRE(registry.view<Position>().size() == 1);
    REQUIRE(registry.get<Position>(next).x == 7.0f);
  }

  SECTION("Reserve Adds No Components") {
    registry.reserve<Position>(1000);
    REQUIRE(registry.view<Position>().size() == 0);
  }
}

TEST_CASE("Registry Entity Validation", "[ecs][registry][valid]") {
  sc::ecs::Registry registry;
  using Traits = entt::entt_traits<entt::entity>;

  SECTION("No Entity") {
    const sc::ecs::Entity none;
    REQUIRE(!none);
    REQUIRE(!registry.valid(none));
    REQUIRE(!registry.valid(entt::null));
    REQUIRE(!registry.valid(entt::tombstone));
    REQUIRE(!registry.destroy(none));
  }

  SECTION("A Created Entity Knows Its Registry") {
    const sc::ecs::Entity single = registry.create();
    REQUIRE(single);
    REQUIRE(single.registry == &registry);
    for (const sc::ecs::Entity &e: make_entities(registry, 10)) {
      REQUIRE(e);
      REQUIRE(e.registry == &registry);
      REQUIRE(registry.valid(e));
    }
  }

  SECTION("A Number That Was Never Handed Out") {
    make_entities(registry, 10);
    REQUIRE(!registry.valid(Traits::construct(1'000'000, 0)));
  }

  SECTION("A Version That Was Never Handed Out") {
    const auto entities = make_entities(registry, 10);
    const entt::entity e = entities[4];
    REQUIRE(registry.valid(e));
    REQUIRE(!registry.valid(Traits::construct(entt::to_entity(e), entt::to_version(e) + 1)));
  }

  SECTION("An Entity Without Components Is Destroyed Like Any Other") {
    const auto entities = make_entities(registry, 3);
    REQUIRE(registry.destroy(entities[1]));
    REQUIRE(registry.valid(entities[0]));
    REQUIRE(!registry.valid(entities[1]));
    REQUIRE(registry.valid(entities[2]));
  }

  SECTION("A Handle Does Not Come Back To Life With Its Number") {
    auto entities = make_entities(registry, 1);
    const sc::ecs::Entity old = entities[0];
    // The number goes around several times.
    for (int round = 0; round < 5; ++round) {
      const sc::ecs::Entity previous = entities[0];
      REQUIRE(registry.destroy(previous));
      registry.create(entities.begin(), entities.end());

      REQUIRE(entt::to_entity(entities[0].entity) == entt::to_entity(old.entity));
      REQUIRE(entities[0].entity != previous.entity);
      REQUIRE(registry.valid(entities[0]));
      REQUIRE(!registry.valid(previous));
      REQUIRE(!registry.valid(old));
      REQUIRE(!registry.destroy(old));
    }
  }

  SECTION("Released Entities") {
    const auto entities = make_entities(registry, 20);
    registry.release(entities.begin(), entities.begin() + 5);
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(registry.valid(entities[i]) == (i >= 5));
    }
  }

  SECTION("Each Registry On Its Own") {
    sc::ecs::Registry other;
    const auto mine = make_entities(registry, 5);
    const auto theirs = make_entities(other, 5);
    REQUIRE(registry.id() != other.id());

    REQUIRE(registry.destroy(mine[2]));
    REQUIRE(other.valid(theirs[2]));
    REQUIRE(theirs[2].registry == &other);
  }
}

TEST_CASE("Registry Validation While Entities Are Created", "[ecs][registry][valid][threading]") {
  constexpr size_t thread_count = 8;
  constexpr size_t per_thread = 3000;

  sc::ecs::Registry registry;
  // Catch2 assertions are not made from other threads.
  std::atomic<size_t> invalid{0};
  {
    std::vector<std::jthread> threads;
    for (size_t t = 0; t < thread_count; ++t) {
      threads.emplace_back([&registry, &invalid] {
        entt::entity previous = entt::null;
        for (size_t i = 0; i < per_thread; ++i) {
          const sc::ecs::Entity e = registry.create();
          if (!registry.valid(e) || (previous != entt::null && !registry.valid(previous))) {
            invalid.fetch_add(1);
          }
          previous = e;
        }
      });
    }
  }
  REQUIRE(invalid.load() == 0);
}
