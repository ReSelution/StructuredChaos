#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "sc/ecs/commands.hpp"
#include "sc/ecs/group.hpp"
#include "sc/ecs/registry.hpp"
#include "sc/ecs/scheduler.hpp"
#include "sc/ecs/view.hpp"
#include "sc/threading/threading.hpp"

namespace {

  using sc::ecs::Commands;
  using sc::ecs::Group;
  using sc::ecs::Registry;
  using sc::ecs::Scheduler;
  using sc::ecs::View;
  using sc::ecs::internal::componentIndex;

  struct Position {
    float x{0.0f};
    float y{0.0f};
  };

  struct Velocity {
    float dx{0.0f};
    float dy{0.0f};
  };

  struct Mass {
    float value{1.0f};
  };

  struct Health {
    int value{100};
  };

  struct Marker {
    int value{0};
  };

  struct Frozen {};

  using Stages = std::vector<std::vector<size_t>>;

  std::vector<sc::ecs::Entity> make_entities(Registry &registry, size_t count) {
    std::vector<sc::ecs::Entity> entities(count);
    registry.create(entities.begin(), entities.end());
    return entities;
  }

} // namespace

TEST_CASE("Registry Groups", "[ecs][registry][group]") {
  Registry registry;
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

  SECTION("A Group Is The Type A System Takes") {
    STATIC_REQUIRE(std::is_same_v<decltype(registry.group<Position, const Velocity>(entt::get<const Mass>)),
                                  Group<entt::owned_t<Position, const Velocity>, entt::get_t<const Mass>>>);
  }

  SECTION("Owning Group Over Existing Components") {
    auto group = registry.group<Position, Velocity>();
    REQUIRE(group.size() == 50);
    for (auto [entity, pos, vel] : group.each()) {
      REQUIRE(static_cast<size_t>(pos.x) % 2 == 0);
      REQUIRE(vel.dy == 2.0f);
    }
  }

  SECTION("Group Follows Later Changes") {
    auto group = registry.group<Position, Velocity>();

    registry.emplace<Velocity>(entities[1]);
    REQUIRE(group.size() == 51);
    registry.erase<Velocity>(entities[0]);
    REQUIRE(group.size() == 50);
    registry.destroy(entities[2]);
    REQUIRE(group.size() == 49);

    // The same group again, not a second one.
    REQUIRE(registry.group<Position, Velocity>().size() == 49);
    REQUIRE(registry.group<Position, const Velocity>().size() == 49);
  }

  SECTION("Const Components Are Read Only") {
    auto group = registry.group<Position, const Velocity>();
    for (auto [entity, pos, vel] : group.each()) {
      STATIC_REQUIRE(!std::is_const_v<std::remove_reference_t<decltype(pos)>>);
      STATIC_REQUIRE(std::is_const_v<std::remove_reference_t<decltype(vel)>>);
      pos.y = vel.dy;
    }
    REQUIRE(registry.get<Position>(entities[0]).y == 2.0f);
    REQUIRE(registry.get<Position>(entities[1]).y == 0.0f);
  }

  SECTION("Partly Owning Group With Exclusion") {
    auto group = registry.group<Position>(entt::get<const Velocity>, entt::exclude<Frozen>);
    REQUIRE(group.size() == 25);
    registry.emplace<Frozen>(entities[2]);
    REQUIRE(group.size() == 24);
  }

  SECTION("Non-Owning Group") {
    auto group = registry.group<>(entt::get<Position, Velocity>);
    REQUIRE(group.size() == 50);
    registry.emplace<Velocity>(entities[1]);
    REQUIRE(group.size() == 51);
  }

  SECTION("The Components Of A Group Become One Family") {
    REQUIRE(registry.family(componentIndex<Position>()) == componentIndex<Position>());
    REQUIRE(registry.family(componentIndex<Velocity>()) == componentIndex<Velocity>());
    // Never used with this registry.
    REQUIRE(registry.family(componentIndex<Health>()) == componentIndex<Health>());

    registry.group<Position>(entt::get<Velocity>, entt::exclude<Frozen>);
    const size_t family = registry.family(componentIndex<Position>());
    REQUIRE(registry.family(componentIndex<Velocity>()) == family);
    REQUIRE(registry.family(componentIndex<Frozen>()) == family);
    REQUIRE(registry.family(componentIndex<Mass>()) != family);

    // A second group that shares a component takes the whole family along.
    registry.reserve<Health>(1);
    registry.group<>(entt::get<Mass, Health>);
    REQUIRE(registry.family(componentIndex<Mass>()) == registry.family(componentIndex<Health>()));
    REQUIRE(registry.family(componentIndex<Mass>()) != registry.family(componentIndex<Position>()));

    registry.group<>(entt::get<Health, Velocity>);
    const size_t merged = registry.family(componentIndex<Position>());
    REQUIRE(registry.family(componentIndex<Velocity>()) == merged);
    REQUIRE(registry.family(componentIndex<Frozen>()) == merged);
    REQUIRE(registry.family(componentIndex<Mass>()) == merged);
    REQUIRE(registry.family(componentIndex<Health>()) == merged);
  }
}

TEST_CASE("Scheduler Systems With Groups", "[ecs][scheduler][group]") {
  sc::threading::init();

  using Moving = Group<entt::owned_t<Position, const Velocity>>;

  Scheduler scheduler;

  SECTION("A Group Is Scheduled Like A View") {
    scheduler.add([](Moving) {});
    scheduler.add([](View<const Velocity>) {});
    scheduler.add([](View<Health>) {});
    scheduler.add([](View<const Position>) {});
    scheduler.add([](Group<entt::owned_t<>, entt::get_t<const Mass>, entt::exclude_t<Frozen>>) {});
    scheduler.add([](View<Frozen>) {});
    REQUIRE(scheduler.stages() == Stages{{0, 1, 2, 4}, {3, 5}});
  }

  SECTION("The Group Is Created Before The Systems Run") {
    Registry registry;
    const auto entities = make_entities(registry, 1000);
    for (size_t i = 0; i < entities.size(); ++i) {
      registry.emplace<Position>(entities[i], static_cast<float>(i), 0.0f);
      registry.emplace<Health>(entities[i]);
      if (i % 2 == 0) {
        registry.emplace<Velocity>(entities[i], 1.0f, 2.0f);
      }
    }

    // Runs next to systems that read the storages the group has to reorder
    // when it is created. Checked afterwards: Catch2 assertions are not meant
    // for systems that run at the same time.
    size_t velocities = 0;
    size_t grouped = 0;
    scheduler.add([&velocities](View<const Velocity> view) { velocities = view.size(); });
    scheduler.add([](View<Health> view) {
      for (auto [entity, health] : view.each()) {
        health.value -= 1;
      }
    });
    scheduler.add([&grouped](Moving group) {
      grouped = group.size();
      for (auto [entity, pos, vel] : group.each()) {
        pos.x += vel.dx;
        pos.y += vel.dy;
      }
    });
    REQUIRE(scheduler.stages() == Stages{{0, 1, 2}});

    scheduler.run(registry);
    scheduler.run(registry);

    REQUIRE(velocities == 500);
    REQUIRE(grouped == 500);
    for (size_t i = 0; i < entities.size(); ++i) {
      const auto &pos = registry.get<Position>(entities[i]);
      REQUIRE(pos.x == static_cast<float>(i) + (i % 2 == 0 ? 2.0f : 0.0f));
      REQUIRE(pos.y == (i % 2 == 0 ? 4.0f : 0.0f));
      REQUIRE(registry.get<Health>(entities[i]).value == 98);
    }
  }
}

TEST_CASE("Commands On The Components Of A Group", "[ecs][commands][group][threading]") {
  sc::threading::init();

  constexpr uint32_t per_system = 4000;

  Registry registry;
  const auto entities = make_entities(registry, per_system);
  for (uint32_t i = 0; i < per_system; ++i) {
    registry.emplace<Health>(entities[i], static_cast<int>(i));
  }
  registry.group<Position, Velocity>(entt::get<Mass>, entt::exclude<Frozen>);

  // Every system adds another component of the group to the same entities, so
  // the group changes with each of them, next to a component outside of it.
  Scheduler scheduler;
  scheduler.add([](View<const Health> view, Commands<Position> commands) {
    for (auto [entity, health] : view.each()) {
      commands.emplace<Position>(entity, static_cast<float>(health.value), 0.0f);
    }
  });
  scheduler.add([](View<const Health> view, Commands<Velocity> commands) {
    for (auto [entity, health] : view.each()) {
      commands.emplace<Velocity>(entity, static_cast<float>(health.value), 1.0f);
    }
  });
  scheduler.add([](View<const Health> view, Commands<Mass, Frozen> commands) {
    for (auto [entity, health] : view.each()) {
      commands.emplace<Mass>(entity, static_cast<float>(health.value));
      if (health.value % 4 == 0) {
        commands.emplace<Frozen>(entity);
      }
    }
  });
  scheduler.add([](View<const Health> view, Commands<Marker> commands) {
    for (auto [entity, health] : view.each()) {
      commands.emplace<Marker>(commands.create(), health.value);
    }
  });
  REQUIRE(scheduler.stages().size() == 1);

  scheduler.run(registry);

  auto group = registry.group<Position, Velocity>(entt::get<Mass>, entt::exclude<Frozen>);
  REQUIRE(group.size() == per_system - per_system / 4);
  for (auto [entity, pos, vel, mass] : group.each()) {
    const int id = registry.get<Health>(entity).value;
    REQUIRE(id % 4 != 0);
    REQUIRE(pos.x == static_cast<float>(id));
    REQUIRE(vel.dx == static_cast<float>(id));
    REQUIRE(mass.value == static_cast<float>(id));
  }
  REQUIRE(registry.view<Marker>().size() == per_system);

  // And out of the group again.
  Scheduler cleanup;
  cleanup.add([](View<const Frozen> view, Commands<Frozen> commands) {
    for (auto entity : view) {
      commands.remove<Frozen>(entity);
    }
  });
  cleanup.add([](View<const Health> view, Commands<Mass> commands) {
    for (auto [entity, health] : view.each()) {
      if (health.value % 2 == 1) {
        commands.remove<Mass>(entity);
      }
      if (health.value % 8 == 0) {
        commands.destroy(entity);
      }
    }
  });
  cleanup.run(registry);

  REQUIRE(group.size() == per_system / 2 - per_system / 8);
  for (auto [entity, pos, vel, mass] : group.each()) {
    const int id = registry.get<Health>(entity).value;
    REQUIRE(id % 2 == 0);
    REQUIRE(id % 8 != 0);
    REQUIRE(pos.x == static_cast<float>(id));
  }
}
