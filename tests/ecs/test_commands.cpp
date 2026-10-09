#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "sc/ecs/commands.hpp"
#include "sc/ecs/registry.hpp"
#include "sc/ecs/resource.hpp"
#include "sc/ecs/scheduler.hpp"
#include "sc/ecs/view.hpp"
#include "sc/threading/threading.hpp"

namespace {

  using sc::ecs::Commands;
  using sc::ecs::Registry;
  using sc::ecs::Scheduler;
  using sc::ecs::View;

  struct Position {
    float x{0.0f};
    float y{0.0f};
  };

  struct Velocity {
    float dx{0.0f};
    float dy{0.0f};
  };

  struct Health {
    int value{100};
  };

  struct Owner {
    uint32_t system{0};
    uint32_t index{0};
  };

  struct Frozen {};

  struct MeshComponent {
    sc::ecs::Resource<std::span<const float>> vertices;
    uint32_t meshId{0};
  };

  using Stages = std::vector<std::vector<size_t>>;

  template <typename View> size_t count(View view) {
    size_t result = 0;
    for ([[maybe_unused]] auto entity : view) {
      ++result;
    }
    return result;
  }

} // namespace

TEST_CASE("Commands Are Made After The Stage", "[ecs][commands]") {
  sc::threading::init();

  Registry registry;
  std::vector<sc::ecs::Entity> entities(100);
  registry.create(entities.begin(), entities.end());
  for (const auto &e : entities) {
    registry.emplace<Position>(e);
  }

  Scheduler scheduler;

  SECTION("Not Visible To The System, Visible To Later Ones") {
    size_t seenWhileRecording = 0;
    size_t seenNextToIt = 0;
    size_t seenAfter = 0;
    scheduler.add([&](View<const Position> positions, Commands<Velocity> commands) {
      for (auto entity : positions) {
        commands.emplace<Velocity>(entity, 1.0f, 2.0f);
      }
      seenWhileRecording = positions.size();
    });
    scheduler.add([&](View<const Health> healths) { seenNextToIt = healths.size(); });
    scheduler.add([&](View<Velocity> velocities) { seenAfter = velocities.size(); });
    // Commands do not keep the first system from running next to the second.
    REQUIRE(scheduler.stages() == Stages{{0, 1, 2}});
    scheduler.run(registry);

    REQUIRE(seenWhileRecording == 100);
    REQUIRE(seenAfter == 0);
    REQUIRE(registry.view<Velocity>().size() == 100);
    REQUIRE(registry.get<Velocity>(entities[42]).dy == 2.0f);
  }

  SECTION("A System In A Later Stage Sees Them") {
    size_t seen = 0;
    scheduler.add([](View<const Position> positions, Commands<Health> commands) {
      for (auto entity : positions) {
        commands.emplace<Health>(entity, 5);
      }
    });
    scheduler.add([](View<Position>) {});
    scheduler.add([&seen](View<const Position>, View<const Health> healths) { seen = healths.size(); });
    REQUIRE(scheduler.stages() == Stages{{0}, {1}, {2}});
    scheduler.run(registry);
    REQUIRE(seen == 100);
  }

  SECTION("In The Order They Were Recorded") {
    const entt::entity e = entities[0];
    scheduler.add([e](Commands<Health, Velocity, Frozen> commands) {
      commands.emplace<Health>(e, 1);
      commands.remove<Health>(e);
      commands.emplace<Health>(e, 2);
      commands.remove<Velocity>(e); // not there, nothing happens
      commands.emplace<Frozen>(e);
    });
    scheduler.run(registry);

    REQUIRE(registry.get<Health>(e).value == 2);
    REQUIRE(registry.view<Health>().size() == 1);
    REQUIRE(registry.view<Frozen>().size() == 1);
    REQUIRE(registry.view<Velocity>().size() == 0);

    // The buffer is empty again: a second run adds the component once more.
    registry.erase<Health>(e);
    registry.erase<Frozen>(e);
    scheduler.run(registry);
    REQUIRE(registry.get<Health>(e).value == 2);
  }

  SECTION("Adding And Removing In Turns") {
    // Every entity gets the component, loses it and gets it again; every
    // second one loses it for good.
    scheduler.add([&](Commands<Health> commands) {
      for (size_t i = 0; i < entities.size(); ++i) {
        commands.emplace<Health>(entities[i], 1);
        commands.remove<Health>(entities[i]);
        commands.emplace<Health>(entities[i], static_cast<int>(i));
        if (i % 2 == 1) {
          commands.remove<Health>(entities[i]);
        }
      }
    });
    for (int run = 0; run < 2; ++run) {
      scheduler.run(registry);

      REQUIRE(registry.view<Health>().size() == entities.size() / 2);
      for (size_t i = 0; i < entities.size(); i += 2) {
        REQUIRE(registry.get<Health>(entities[i]).value == static_cast<int>(i));
        registry.erase<Health>(entities[i]);
      }
    }
  }

  SECTION("Systems Of One Stage In The Order They Were Added") {
    const entt::entity e = entities[0];
    scheduler.add([e](View<const Position>, Commands<Health> commands) { commands.emplace<Health>(e, 1); });
    scheduler.add([e](View<const Position>, Commands<Health> commands) { commands.remove<Health>(e); });
    scheduler.add([e](View<const Position>, Commands<Health> commands) { commands.emplace<Health>(e, 3); });
    REQUIRE(scheduler.stages() == Stages{{0, 1, 2}});

    for (int run = 0; run < 50; ++run) {
      scheduler.run(registry);
      REQUIRE(registry.get<Health>(e).value == 3);
      registry.erase<Health>(e);
    }
  }

  SECTION("Destroying Entities") {
    scheduler.add([](View<const Position> positions, Commands<Health> commands) {
      size_t i = 0;
      for (auto entity : positions) {
        if (i++ % 2 == 0) {
          commands.emplace<Health>(entity, 1);
          commands.destroy(entity);
          if (i % 4 == 1) {
            // Twice by the same system.
            commands.destroy(entity);
          }
        }
      }
      // Still there: nothing happens while the system runs.
      REQUIRE(positions.size() == 100);
    });
    // And one of them once more by another system.
    scheduler.add([](View<const Position> positions, Commands<> commands) {
      for (auto entity : positions) {
        commands.destroy(entity);
        break;
      }
    });
    scheduler.run(registry);

    REQUIRE(registry.view<Position>().size() == 50);
    REQUIRE(registry.view<Health>().size() == 0);
    REQUIRE(count(registry.view<Position>()) == 50);
  }

  SECTION("Created Entities Exist Right Away") {
    std::vector<entt::entity> created;
    scheduler.add([&](Commands<Health> commands) {
      for (int i = 0; i < 10; ++i) {
        const auto e = commands.create();
        REQUIRE(registry.valid(e));
        commands.emplace<Health>(e, i);
        created.push_back(e);
      }
    });
    scheduler.run(registry);

    REQUIRE(registry.view<Health>().size() == 10);
    REQUIRE(registry.get<Health>(created[7]).value == 7);
  }

  SECTION("A Range Of Created Entities Exists Right Away") {
    std::array<sc::ecs::Entity, 100> created;
    scheduler.add([&](Commands<Health> commands) {
      commands.create(created.begin(), created.end());
      for (size_t i = 0; i < created.size(); ++i) {
        REQUIRE(created[i].registry == &registry);
        REQUIRE(registry.valid(created[i]));
        commands.emplace<Health>(created[i], static_cast<int>(i));
      }
    });
    scheduler.run(registry);

    REQUIRE(registry.view<Health>().size() == created.size());
    REQUIRE(registry.get<Health>(created[7]).value == 7);
  }

  SECTION("Created Entities Belong To The Registry The Scheduler Runs On") {
    // More than one block of entities per run, and one that is left half used.
    constexpr int per_run = 100;
    Registry *current = nullptr;
    scheduler.add([&](Commands<Health> commands) {
      for (int i = 0; i < per_run; ++i) {
        const auto e = commands.create();
        REQUIRE(e.registry == current);
        REQUIRE(current->valid(e));
        commands.emplace<Health>(e, i);
      }
    });

    Registry other;
    for (Registry *target : {&registry, &other, &registry}) {
      current = target;
      scheduler.run(*target);
    }
    REQUIRE(registry.view<Health>().size() == 2 * per_run);
    REQUIRE(other.view<Health>().size() == per_run);
  }

  SECTION("Components With Resources") {
    constexpr size_t vertex_count = 16;
    scheduler.add([](View<const Position> positions, Commands<MeshComponent> commands) {
      uint32_t id = 0;
      for (auto entity : positions) {
        std::array<float, vertex_count> vertices{};
        vertices.fill(static_cast<float>(id));
        commands.emplace<MeshComponent>(entity, std::span<const float>(vertices), id);
        ++id;
      }
    });
    scheduler.run(registry);

    size_t meshes = 0;
    for (auto [entity, mesh] : registry.view<MeshComponent>().each()) {
      REQUIRE(mesh.vertices.size() == vertex_count);
      REQUIRE(mesh.vertices[0] == static_cast<float>(mesh.meshId));
      REQUIRE(mesh.vertices[vertex_count - 1] == static_cast<float>(mesh.meshId));
      ++meshes;
    }
    REQUIRE(meshes == 100);

    // Removing releases the buffers, adding again works on the same heap.
    Scheduler cleanup;
    cleanup.add([](View<const MeshComponent> meshes, Commands<MeshComponent> commands) {
      for (auto entity : meshes) {
        commands.remove<MeshComponent>(entity);
      }
    });
    cleanup.run(registry);
    REQUIRE(count(registry.view<MeshComponent>()) == 0);

    scheduler.run(registry);
    REQUIRE(count(registry.view<MeshComponent>()) == 100);
  }

  SECTION("An Exception Drops What The Stage Recorded") {
    const entt::entity e = entities[0];
    bool fail = true;
    scheduler.add([e](View<const Position>, Commands<Health> commands) { commands.emplace<Health>(e, 1); });
    scheduler.add([&fail](View<const Position>) {
      if (fail) {
        throw std::runtime_error("broken system");
      }
    });

    REQUIRE_THROWS_AS(scheduler.run(registry), std::runtime_error);
    REQUIRE(registry.view<Health>().size() == 0);

    fail = false;
    scheduler.run(registry);
    REQUIRE(registry.view<Health>().size() == 1);
  }
}

TEST_CASE("Commands From Systems Running At The Same Time", "[ecs][commands][threading]") {
  sc::threading::init();

  constexpr uint32_t system_count = 6;
  constexpr uint32_t per_system = 5000;

  Registry registry;
  Scheduler scheduler;
  for (uint32_t system = 0; system < system_count; ++system) {
    scheduler.add([system](Commands<Owner, Position, Velocity, Frozen, Health> commands) {
      for (uint32_t i = 0; i < per_system; ++i) {
        const auto e = commands.create();
        commands.emplace<Owner>(e, system, i);
        commands.emplace<Position>(e, static_cast<float>(system), static_cast<float>(i));
        if (i % 2 == 0) {
          commands.emplace<Velocity>(e);
        }
        if (i % 4 == 0) {
          commands.emplace<Frozen>(e);
        }
        if (i % 5 == 0) {
          commands.emplace<Health>(e, 1);
          commands.destroy(e);
        }
      }
    });
  }
  REQUIRE(scheduler.stages().size() == 1);

  for (uint32_t run = 1; run <= 3; ++run) {
    scheduler.run(registry);

    const size_t alive = static_cast<size_t>(run) * system_count * (per_system - per_system / 5);
    REQUIRE(registry.view<Owner>().size() == alive);
    REQUIRE(registry.view<Position>().size() == alive);
    REQUIRE(registry.view<Health>().size() == 0);
    REQUIRE(count(registry.view<Owner, Position, Velocity>()) == alive / 2);
  }

  std::vector<uint32_t> perSystem(system_count, 0);
  for (auto [entity, owner, pos] : registry.view<const Owner, const Position>().each()) {
    REQUIRE(pos.x == static_cast<float>(owner.system));
    REQUIRE(pos.y == static_cast<float>(owner.index));
    REQUIRE(owner.index % 5 != 0);
    ++perSystem[owner.system];
  }
  for (const uint32_t n : perSystem) {
    REQUIRE(n == 3 * (per_system - per_system / 5));
  }
}

TEST_CASE("Commands Give Spare Entities Back", "[ecs][commands]") {
  sc::threading::init();

  // What a system that creates a single entity leaves in its buffer.
  constexpr size_t SPARE = sc::ecs::internal::CommandBufferBase::ENTITY_BLOCK_SIZE - 1;
  const auto spawn = [](Commands<Position> commands) { commands.emplace<Position>(commands.create()); };
  // Whether the registry hands out the spare entities again: an entity that
  // was given back comes with a new version.
  const auto reused = [](Registry &registry) {
    std::vector<sc::ecs::Entity> entities(SPARE);
    registry.create(entities.begin(), entities.end());
    for (const sc::ecs::Entity &entity : entities) {
      if (entt::to_version(entity.entity) == 0) {
        return false;
      }
    }
    return true;
  };

  SECTION("Kept For The Next Run") {
    Registry registry;
    Scheduler scheduler;
    scheduler.add(spawn);
    scheduler.run(registry);
    scheduler.run(registry);
    REQUIRE(registry.view<Position>().size() == 2);
    REQUIRE(!reused(registry));
  }

  SECTION("When The Scheduler Goes Away") {
    Registry registry;
    {
      Scheduler scheduler;
      scheduler.add(spawn);
      scheduler.run(registry);
    }
    REQUIRE(registry.view<Position>().size() == 1);
    REQUIRE(reused(registry));
  }

  SECTION("When The Scheduler Runs On Another Registry") {
    Registry first;
    Registry second;
    Scheduler scheduler;
    scheduler.add(spawn);
    scheduler.run(first);
    scheduler.run(second);
    REQUIRE(reused(first));
    REQUIRE(!reused(second));
  }

  SECTION("Not To A Registry That Is Gone") {
    Scheduler scheduler;
    scheduler.add(spawn);
    {
      Registry gone;
      scheduler.run(gone);
    }
    Registry registry;
    scheduler.run(registry);
    REQUIRE(registry.view<Position>().size() == 1);
    {
      Registry gone;
      scheduler.run(gone);
    }
    // And the scheduler outlives the registry it ran on last.
  }
}

TEST_CASE("Commands Emplace", "[ecs][commands][emplace]") {
  sc::threading::init();

  Registry registry;
  Scheduler scheduler;

  SECTION("Builds The Components Of A New Entity From Their Arguments") {
    scheduler.add([](Commands<Position, Velocity, Health, Frozen> commands) {
      const auto e = commands.create();
      commands.emplace<Position>(e, 1.0f, 2.0f);
      commands.emplace<Velocity>(e, 3.0f, 4.0f);
      // Without arguments: the defaults of the component.
      commands.emplace<Health>(e);
      commands.emplace<Frozen>(e);
    });
    scheduler.run(registry);

    REQUIRE(count(registry.view<Position, Velocity, Health, Frozen>()) == 1);
    for (auto [entity, pos, vel, health] : registry.view<Position, Velocity, Health, Frozen>().each()) {
      REQUIRE(registry.valid(entity));
      REQUIRE(pos.x == 1.0f);
      REQUIRE(pos.y == 2.0f);
      REQUIRE(vel.dx == 3.0f);
      REQUIRE(vel.dy == 4.0f);
      REQUIRE(health.value == 100);
    }
  }

  SECTION("More New Entities Than One Block") {
    constexpr size_t count_per_run = 3 * sc::ecs::internal::CommandBufferBase::ENTITY_BLOCK_SIZE + 7;
    uint32_t next = 0;
    scheduler.add([&next](Commands<Owner> commands) {
      for (size_t i = 0; i < count_per_run; ++i) {
        commands.emplace<Owner>(commands.create(), 0u, next++);
      }
    });
    scheduler.run(registry);
    scheduler.run(registry);

    // Every entity is another one and has the value it was created with.
    std::vector<bool> seen(2 * count_per_run, false);
    auto owners = registry.view<const Owner>();
    REQUIRE(owners.size() == 2 * count_per_run);
    for (auto [entity, owner] : owners.each()) {
      REQUIRE(registry.valid(entity));
      REQUIRE(owner.index < seen.size());
      REQUIRE(!seen[owner.index]);
      seen[owner.index] = true;
    }
  }

  SECTION("On The Entities A View Finds, Once") {
    std::vector<sc::ecs::Entity> entities(200);
    registry.create(entities.begin(), entities.end());
    for (size_t i = 0; i < entities.size(); ++i) {
      registry.emplace<Position>(entities[i], static_cast<float>(i), 0.0f);
    }

    // Finds nothing left to do in the runs after the first one.
    scheduler.add([](View<const Position, sc::ecs::Exclude<Health>> view, Commands<Health> commands) {
      for (auto [entity, pos] : view.each()) {
        commands.emplace<Health>(entity, static_cast<int>(pos.x));
      }
    });
    for (int run = 0; run < 5; ++run) {
      scheduler.run(registry);
      REQUIRE(registry.view<Health>().size() == entities.size());
    }
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(registry.get<Health>(entities[i]).value == static_cast<int>(i));
    }
  }

  SECTION("Does Not Touch The Components An Entity Has Already") {
    const auto e = registry.create();
    registry.emplace<Position>(e, 1.0f, 2.0f);
    const Position *before = &registry.get<Position>(e);

    scheduler.add([e](Commands<Velocity> commands) { commands.emplace<Velocity>(e, 5.0f, 6.0f); });
    scheduler.run(registry);

    REQUIRE(&registry.get<Position>(e) == before);
    REQUIRE(registry.get<Position>(e).x == 1.0f);
    REQUIRE(registry.get<Velocity>(e).dy == 6.0f);
  }

  SECTION("On An Entity Destroyed By The Same Stage Leaves Nothing Behind") {
    const auto e = registry.create();
    scheduler.add([e](Commands<Position> commands) { commands.emplace<Position>(e, 1.0f, 1.0f); });
    scheduler.add([e](Commands<> commands) { commands.destroy(e); });
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
    scheduler.run(registry);

    REQUIRE(!registry.valid(e));
    REQUIRE(registry.view<Position>().size() == 0);
  }
}
