#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "sc/ecs2/scheduler.hpp"
#include "sc/ecs2/world.hpp"
#include "sc/threading/threading.hpp"

namespace {

  using sc::ecs2::Change;
  using sc::ecs2::Commands;
  using sc::ecs2::Entity;
  using sc::ecs2::Exclude;
  using sc::ecs2::PAGE_SIZE;
  using sc::ecs2::Query;
  using sc::ecs2::Scheduler;
  using sc::ecs2::World;

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

  struct Frozen {};

  struct Spawner {
    int count{0};
  };

  using Stages = std::vector<std::vector<size_t>>;

  void move(Query<Position, const Velocity> query) {
    query.each([](Position &pos, const Velocity &vel) {
      pos.x += vel.dx;
      pos.y += vel.dy;
    });
  }

  struct Doubler {
    void operator()(const Query<Position> &query) const {
      query.each([](Position &pos) { pos.y *= 2.0f; });
    }
  };

} // namespace

TEST_CASE("ECS2 Scheduler Stages", "[ecs2][scheduler]") {
  Scheduler scheduler;

  SECTION("Nothing To Run") { REQUIRE(scheduler.stages().empty()); }

  SECTION("Readers Of The Same Component Run Together") {
    scheduler.add([](Query<const Position>) {});
    scheduler.add([](Query<const Position, const Velocity>) {});
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
  }

  SECTION("Writers Of Different Components Run Together") {
    scheduler.add([](Query<Position, const Velocity>) {});
    scheduler.add([](Query<Health, const Velocity>) {});
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
  }

  SECTION("A Writer Waits For Earlier Readers And Writers") {
    scheduler.add([](Query<const Position>) {});
    scheduler.add([](Query<Position>) {});
    scheduler.add([](Query<Position>) {});
    scheduler.add([](Query<const Position>) {});
    REQUIRE(scheduler.stages() == Stages{{0}, {1}, {2}, {3}});
  }

  SECTION("A System Only Waits For The Ones It Conflicts With") {
    scheduler.add([](Query<Position>) {});
    scheduler.add([](Query<Position>) {});
    scheduler.add([](Query<Health>) {});
    REQUIRE(scheduler.stages() == Stages{{0, 2}, {1}});
  }

  SECTION("An Excluded Component Counts As Read") {
    scheduler.add([](Query<Position, Exclude<Frozen>>) {});
    scheduler.add([](Query<const Health>, Change<Frozen>) {});
    scheduler.add([](Query<const Health, Exclude<Frozen>>) {});
    REQUIRE(scheduler.stages() == Stages{{0}, {1}, {2}});
  }

  SECTION("Changing A Component Counts As Writing It") {
    scheduler.add([](Query<const Position>, Change<Health>) {});
    scheduler.add([](Query<const Health>) {});
    scheduler.add([](Query<const Position>, Change<Frozen>) {});
    REQUIRE(scheduler.stages() == Stages{{0, 2}, {1}});
  }

  SECTION("Commands Keep Nobody Waiting") {
    scheduler.add([](Query<const Position>, Commands) {});
    scheduler.add([](Query<const Position>, Commands) {});
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
  }

  SECTION("A System That Takes The World Or Nothing Runs Alone") {
    scheduler.add([](Query<const Position>) {});
    scheduler.add([](World &) {}, "load");
    scheduler.add([](Query<const Health>) {});
    scheduler.add([] {});
    scheduler.add([](Query<const Health>) {});
    REQUIRE(scheduler.stages() == Stages{{0}, {1}, {2}, {3}, {4}});
    REQUIRE(scheduler.name(1) == "load");
    REQUIRE(scheduler.size() == 5);
  }

  SECTION("The Parameters Of A Loop Say What It Uses") {
    scheduler.addParallel([](Position &, const Velocity &) {});
    scheduler.addParallel([](Health &, Velocity) {});
    scheduler.addParallel([](Entity, Change<Frozen>, const Health &) {});
    scheduler.addParallel<Query<Velocity, Exclude<Frozen>>>([](Velocity &) {});
    REQUIRE(scheduler.stages() == Stages{{0, 1}, {2}, {3}});
  }
}

TEST_CASE("ECS2 Scheduler Runs Systems", "[ecs2][scheduler][threading]") {
  sc::threading::init();

  World world;
  std::vector<Entity> entities;
  for (size_t i = 0; i < 2 * PAGE_SIZE + 300; ++i) {
    entities.push_back(world.create());
    world.emplace<Position>(entities.back(), static_cast<float>(i), 0.0f);
    world.emplace<Health>(entities.back());
    if (i % 3 != 0) {
      world.emplace<Velocity>(entities.back(), 1.0f, 2.0f);
    }
  }

  Scheduler scheduler;

  SECTION("Functions, Function Objects And Lambdas") {
    scheduler.add(move);
    scheduler.add(Doubler{});
    scheduler.add([](Query<Health> query) { query.each([](Health &health) { health.value += 1; }); });
    REQUIRE(scheduler.stages() == Stages{{0, 2}, {1}});
    scheduler.run(world);
    scheduler.run(world);
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.get<Position>(entities[i]).y == (i % 3 != 0 ? 12.0f : 0.0f));
      REQUIRE(world.get<Health>(entities[i]).value == 102);
    }
  }

  SECTION("Loops On Several Threads") {
    scheduler.addParallel([](Position &pos, const Velocity &vel) { pos.y += vel.dy; }, "move");
    scheduler.addParallel([](Entity entity, Health &health, Velocity vel) {
      health.value += static_cast<int>(vel.dx) + static_cast<int>(sc::ecs2::indexOf(entity) % 2);
    });
    scheduler.addParallel<Query<Position, Exclude<Velocity>>>([](Position &pos) { pos.y -= 1.0f; });
    REQUIRE(scheduler.stages() == Stages{{0, 1}, {2}});
    scheduler.run(world);
    for (size_t i = 0; i < entities.size(); ++i) {
      const bool moving = i % 3 != 0;
      REQUIRE(world.get<Position>(entities[i]).y == (moving ? 2.0f : -1.0f));
      REQUIRE(world.get<Health>(entities[i]).value ==
              (moving ? 101 + static_cast<int>(sc::ecs2::indexOf(entities[i]) % 2) : 100));
    }
  }

  SECTION("A Loop Adds And Removes Components Of Its Entity") {
    scheduler.addParallel([](Entity entity, Change<Frozen, Velocity> change, const Position &pos) {
      const auto i = static_cast<size_t>(pos.x);
      if (i % 2 == 0) {
        change.emplace<Frozen>(entity);
      }
      if (i % 3 == 1) {
        change.erase<Velocity>(entity);
      }
    });
    // Sees what the loop did.
    std::atomic<size_t> frozen = 0;
    scheduler.addParallel<Query<const Frozen, const Position>>([&](const Position &) { ++frozen; });
    REQUIRE(scheduler.stages() == Stages{{0}, {1}});
    scheduler.run(world);

    REQUIRE(frozen == (entities.size() + 1) / 2);
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.contains<Frozen>(entities[i]) == (i % 2 == 0));
      REQUIRE(world.contains<Velocity>(entities[i]) == (i % 3 == 2));
    }
  }

  SECTION("Entities Are Destroyed Once The Stage Is Over") {
    std::atomic<size_t> seen = 0;
    scheduler.addParallel([](Entity entity, Commands commands, const Position &pos) {
      if (static_cast<size_t>(pos.x) % 2 == 0) {
        commands.destroy(entity);
        // Twice does no harm.
        commands.destroy(entity);
      }
    });
    // Next to it, so every entity is still there.
    scheduler.addParallel([&](const Health &) { ++seen; });
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
    scheduler.run(world);

    REQUIRE(seen == entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.valid(entities[i]) == (i % 2 != 0));
    }
    size_t left = 0;
    world.each<const Position>([&](const Position &) { ++left; });
    REQUIRE(left == entities.size() / 2);

    // Nothing is left over for the next run.
    seen = 0;
    scheduler.run(world);
    REQUIRE(seen == entities.size() / 2);
  }

  SECTION("Threads Create Entities With Components") {
    constexpr int perSpawner = 500;
    for (size_t i = 0; i < entities.size(); i += 100) {
      world.emplace<Spawner>(entities[i], perSpawner);
    }
    const size_t spawners = (entities.size() + 99) / 100;
    // Changes its own entity and the ones it creates, which must not get in
    // each other's way.
    scheduler.addParallel([](Entity entity, Change<Frozen, Velocity> change, const Position &) {
      if (change.contains<Frozen>(entity)) {
        change.erase<Frozen>(entity);
      } else {
        change.emplace<Frozen>(entity);
      }
    });
    scheduler.addParallel([](Change<Frozen, Velocity> change, const Spawner &spawner) {
      for (int i = 0; i < spawner.count; ++i) {
        const Entity spawned = change.create();
        change.emplace<Frozen>(spawned);
        change.emplace<Velocity>(spawned, 7.0f, 7.0f);
      }
    });
    // Twice: back to what it was for the first loop.
    scheduler.addParallel([](Entity entity, Change<Frozen> change, const Position &) {
      if (change.contains<Frozen>(entity)) {
        change.erase<Frozen>(entity);
      } else {
        change.emplace<Frozen>(entity);
      }
    });
    REQUIRE(scheduler.stages() == Stages{{0}, {1}, {2}});
    const size_t before = (entities.size() - (entities.size() + 2) / 3);
    scheduler.run(world);

    size_t spawned = 0;
    world.each<const Frozen, const Velocity>(
        [&](Entity entity, const Velocity &vel) { spawned += world.valid(entity) && vel.dx == 7.0f; });
    REQUIRE(spawned == spawners * perSpawner);
    size_t velocities = 0;
    world.each<const Velocity>([&](const Velocity &) { ++velocities; });
    REQUIRE(velocities == before + spawners * perSpawner);
  }

  SECTION("A System That Takes The World May Do Anything") {
    scheduler.add([&](World &all) {
      for (size_t i = 0; i < entities.size(); i += 2) {
        all.destroy(entities[i]);
      }
    });
    scheduler.addParallel([](Health &health) { health.value += 1; });
    scheduler.run(world);
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.valid(entities[i]) == (i % 2 != 0));
      if (i % 2 != 0) {
        REQUIRE(world.get<Health>(entities[i]).value == 101);
      }
    }
  }

  SECTION("An Exception Ends The Run After Its Stage") {
    std::atomic<int> ran = 0;
    scheduler.addParallel([](Entity entity, Commands commands, const Position &) { commands.destroy(entity); });
    scheduler.add([&](Query<const Health>) {
      ++ran;
      throw std::runtime_error("failed");
    });
    scheduler.add([&](Query<const Velocity>) { ++ran; });
    scheduler.add([&](Query<Health>) { ++ran; });
    REQUIRE(scheduler.stages() == Stages{{0, 1, 2}, {3}});
    REQUIRE_THROWS_AS(scheduler.run(world), std::runtime_error);
    // The systems next to it finished, the later one did not start, and what
    // was recorded is dropped.
    REQUIRE(ran == 2);
    for (const Entity entity : entities) {
      REQUIRE(world.valid(entity));
    }
  }
}
