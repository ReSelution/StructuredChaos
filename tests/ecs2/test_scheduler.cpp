#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "sc/ecs2/scheduler.hpp"
#include "sc/ecs2/world.hpp"
#include "sc/threading/threading.hpp"

namespace {

  using sc::ecs2::Change;
  using sc::ecs2::Commands;
  using sc::ecs2::Entity;
  using sc::ecs2::Exclude;
  using sc::ecs2::Modify;
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

  struct Target {
    int value{0};
  };

  // Owns something, which shows whether every one that was made is gone.
  struct Named {
    std::string name;
    std::shared_ptr<int> alive;
  };

  // Cannot be given to an entity while told so.
  struct Fragile {
    static inline bool breaks = false;

    int value{0};

    explicit Fragile(int number) : value(number) {}
    // Throws on purpose: a component whose move fails.
    // NOLINTNEXTLINE(bugprone-exception-escape,performance-noexcept-move-constructor)
    Fragile(Fragile &&other) : value(other.value) {
      if (breaks) {
        throw std::runtime_error("broken");
      }
    }
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

  SECTION("Modifying A Component Counts As Writing It") {
    scheduler.add([](Query<const Position>, Modify<Health>) {});
    scheduler.add([](Query<const Health>) {});
    scheduler.add([](Query<const Position>, Modify<Spawner, Target>) {});
    scheduler.addParallel([](Modify<Target>, const Velocity &) {});
    REQUIRE(scheduler.stages() == Stages{{0, 2}, {1, 3}});
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
  for (size_t i = 0; i < (2 * PAGE_SIZE) + 300; ++i) {
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
    const size_t before = (entities.size() - ((entities.size() + 2) / 3));
    scheduler.run(world);

    size_t spawned = 0;
    world.each<const Frozen, const Velocity>(
        [&](Entity entity, const Velocity &vel) { spawned += world.valid(entity) && vel.dx == 7.0f; });
    REQUIRE(spawned == spawners * perSpawner);
    size_t velocities = 0;
    world.each<const Velocity>([&](const Velocity &) { ++velocities; });
    REQUIRE(velocities == before + (spawners * perSpawner));
  }

  SECTION("Creating And Destroying Run After Run Does Not Use Up The Indices") {
    for (size_t i = 0; i < entities.size(); i += 100) {
      world.emplace<Spawner>(entities[i], 3);
    }
    const size_t spawners = (entities.size() + 99) / 100;
    // Every run destroys what the one before created.
    scheduler.addParallel<Query<const Frozen>>([](Entity entity, Commands commands) { commands.destroy(entity); });
    scheduler.addParallel([](Change<Frozen> change, const Spawner &spawner) {
      for (int i = 0; i < spawner.count; ++i) {
        change.emplace<Frozen>(change.create());
      }
    });
    REQUIRE(scheduler.stages() == Stages{{0}, {1}});

    // Far more runs than the pages below: an index lost per run would show
    // as more pages. Not more runs than there are pages, which with 2^20
    // of them took minutes.
    const size_t runs = 8192;
    for (size_t run = 0; run < runs; ++run) {
      scheduler.run(world);
    }
    size_t spawned = 0;
    world.each<const Frozen>([&](Entity entity) { spawned += world.valid(entity); });
    REQUIRE(spawned == spawners * 3);
    // A page for every thread at most, next to the first ones: after that
    // the indices of the destroyed entities do.
    REQUIRE(world.pageCount() <= 3 + sc::threading::threadSlots());
    for (const Entity entity: entities) {
      REQUIRE(world.valid(entity));
    }
  }

  SECTION("A Loop Changes Its Entity Next To Entities Created In The Same Page") {
    // Indices all over the pages for the new entities to get.
    for (size_t i = 1; i < entities.size(); i += 2) {
      world.destroy(entities[i]);
    }
    const size_t left = (entities.size() + 1) / 2;
    const uint32_t pages = world.pageCount();
    // Every thread adds the same components to its own entity and to the one
    // it creates, which is in the page of some thread or other.
    scheduler.addParallel([](Entity entity, Change<Frozen, Spawner> change, const Position &pos) {
      change.emplace<Frozen>(entity);
      change.emplace<Spawner>(entity, static_cast<int>(pos.x));
      const Entity spawned = change.create();
      change.emplace<Frozen>(spawned);
      change.emplace<Spawner>(spawned, -1);
    });
    // And takes them away again, the last ones of a page among them.
    scheduler.addParallel([](Entity entity, Change<Frozen, Spawner> change, const Position &) {
      change.erase<Frozen>(entity);
      change.erase<Spawner>(entity);
    });
    REQUIRE(scheduler.stages() == Stages{{0}, {1}});
    scheduler.run(world);

    REQUIRE(world.pageCount() == pages);
    size_t created = 0;
    world.each<const Frozen, const Spawner>([&](Entity entity, const Spawner &spawner) {
      created += world.valid(entity) && !world.contains<Position>(entity) && spawner.count == -1;
    });
    REQUIRE(created == left);
    size_t frozen = 0;
    world.each<const Frozen>([&](Entity) { ++frozen; });
    REQUIRE(frozen == left);
    for (size_t i = 0; i < entities.size(); i += 2) {
      REQUIRE_FALSE(world.contains<Spawner>(entities[i]));
    }
  }

  SECTION("A Loop Changes Entities That Other Threads Go For As Well") {
    // Every entity hits one of a few, as do many on other threads.
    constexpr size_t targets = 10;
    scheduler.addParallel([&](Change<Spawner, Frozen> change, const Position &pos) {
      const Entity target = entities[static_cast<size_t>(pos.x) % targets];
      change.tryEmplace<Spawner>(target, 5);
      change.tryEmplace<Frozen>(target);
    });
    std::atomic<size_t> thawed = 0;
    scheduler.addParallel([&](Change<Frozen> change, const Position &pos) {
      thawed += change.erase<Frozen>(entities[static_cast<size_t>(pos.x) % targets]);
    });
    REQUIRE(scheduler.stages() == Stages{{0}, {1}});
    scheduler.run(world);

    REQUIRE(world.column<Spawner>().page(0).count == targets);
    REQUIRE(thawed == targets);
    REQUIRE(world.column<Frozen>().pages() == 0);
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.contains<Spawner>(entities[i]) == (i < targets));
    }
    for (size_t i = 0; i < targets; ++i) {
      REQUIRE(world.get<Spawner>(entities[i]).count == 5);
    }
  }

  SECTION("A Loop Modifies Entities That Other Threads Go For As Well") {
    // Every entity hits one of a few, as do many on other threads, and
    // every hit counts.
    constexpr size_t targets = 10;
    std::atomic<size_t> missed = 0;
    scheduler.addParallel([&](Modify<Health, Spawner> modify, const Position &pos) {
      const Entity target = entities[static_cast<size_t>(pos.x) % targets];
      modify(target, [](Health &health) { health.value -= 1; });
      // No entity has one.
      missed += !modify(target, [](Spawner &spawner) { ++spawner.count; });
    });
    scheduler.run(world);
    scheduler.run(world);

    REQUIRE(missed == 2 * entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
      // How many entities hit it, twice each.
      const int hits = i < targets ? static_cast<int>((entities.size() - i + targets - 1) / targets) : 0;
      REQUIRE(world.get<Health>(entities[i]).value == 100 - (2 * hits));
    }
  }

  SECTION("A Component Is Removed Once The Stage Is Over") {
    constexpr size_t targets = 10;
    for (size_t i = 0; i < targets; ++i) {
      world.emplace<Spawner>(entities[i], 7);
    }
    // Every entity uses the component of one of a few and wants it gone, as
    // do many on other threads.
    std::atomic<size_t> used = 0;
    scheduler.addParallel([&](Commands commands, Query<const Spawner> query, const Position &pos) {
      const Entity target = entities[static_cast<size_t>(pos.x) % targets];
      used += query.contains<Spawner>(target) && query.get<const Spawner>(target).count == 7;
      commands.erase<Spawner>(target);
      // One it does not have, and one of an entity that will be gone.
      commands.erase<Frozen>(target);
      if (static_cast<size_t>(pos.x) >= entities.size() - 5) {
        commands.erase<Health>(entities[entities.size() - 1]);
        commands.destroy(entities[entities.size() - 1]);
      }
    });
    // Next to it, although it reads the component: it is still there.
    std::atomic<size_t> seen = 0;
    scheduler.addParallel([&](const Spawner &spawner) { seen += spawner.count == 7; });
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
    scheduler.run(world);

    REQUIRE(used == entities.size());
    REQUIRE(seen == targets);
    REQUIRE(world.column<Spawner>().pages() == world.column<Spawner>().spares());
    REQUIRE_FALSE(world.valid(entities.back()));
    for (size_t i = 0; i + 1 < entities.size(); ++i) {
      REQUIRE(world.contains<Health>(entities[i]));
    }

    // Nothing is left over for the next run, and what comes back stays.
    world.emplace<Spawner>(entities[0], 7);
    scheduler.run(world);
    REQUIRE(seen == targets + 1);
    REQUIRE_FALSE(world.contains<Spawner>(entities[0]));
  }

  SECTION("A Component Is Added Once The Stage Is Over") {
    constexpr size_t targets = 10;
    std::atomic<size_t> seen = 0;
    scheduler.addParallel([&](Entity entity, Commands commands, const Position &pos) {
      commands.emplace<Spawner>(entity, static_cast<int>(pos.x));
      commands.emplace<Frozen>(entity);
      // One that many on other threads want to give as well.
      commands.emplace<Target>(entities[static_cast<size_t>(pos.x) % targets], 7);
    });
    // Next to it, so no entity has any of them yet.
    scheduler.addParallel<Query<const Position, Exclude<Spawner, Frozen, Target>>>([&](const Position &) { ++seen; });
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
    scheduler.run(world);

    REQUIRE(seen == entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.get<Spawner>(entities[i]).count == static_cast<int>(i));
      REQUIRE(world.contains<Frozen>(entities[i]));
      REQUIRE(world.contains<Target>(entities[i]) == (i < targets));
    }
    REQUIRE(world.get<Target>(entities[0]).value == 7);

    // An entity that has the component keeps the one it has.
    world.get<Spawner>(entities[0]).count = -1;
    seen = 0;
    scheduler.run(world);
    REQUIRE(seen == 0);
    REQUIRE(world.get<Spawner>(entities[0]).count == -1);
  }

  SECTION("Threads Outside The Pool Record Through Commands") {
    constexpr size_t threadCount = 8;
    scheduler.add([&](Commands commands) {
      std::vector<std::jthread> threads;
      for (size_t t = 0; t < threadCount; ++t) {
        threads.emplace_back([&, t] {
          for (size_t i = t; i < entities.size(); i += threadCount) {
            commands.emplace<Spawner>(entities[i], static_cast<int>(i));
            if (i % 5 == 0) {
              commands.destroy(entities[i]);
            }
          }
        });
      }
    });
    scheduler.run(world);

    for (size_t i = 0; i < entities.size(); ++i) {
      if (i % 5 == 0) {
        REQUIRE_FALSE(world.valid(entities[i]));
      } else {
        REQUIRE(world.get<Spawner>(entities[i]).count == static_cast<int>(i));
      }
    }
  }

  SECTION("Recorded Components Are Added And Removed In Their Order") {
    world.emplace<Spawner>(entities[0], 1);
    world.emplace<Spawner>(entities[1], 1);
    const Entity gone = entities[2];
    Entity made = sc::ecs2::null;
    scheduler.add([&](Commands commands) {
      // Replaced, gone again, and of an entity that is there right away.
      commands.erase<Spawner>(entities[0]);
      commands.emplace<Spawner>(entities[0], 2);
      commands.emplace<Target>(entities[1], 3);
      commands.erase<Target>(entities[1]);
      made = commands.create();
      commands.emplace<Spawner>(made, 4);
      // Nothing for an entity that is no more, or about to be.
      commands.emplace<Spawner>(gone, 5);
      commands.destroy(entities[3]);
      commands.emplace<Spawner>(entities[3], 6);
    });
    world.destroy(gone);
    scheduler.run(world);

    REQUIRE(world.get<Spawner>(entities[0]).count == 2);
    REQUIRE_FALSE(world.contains<Target>(entities[1]));
    REQUIRE(world.valid(made));
    REQUIRE(world.get<Spawner>(made).count == 4);
    REQUIRE_FALSE(world.valid(entities[3]));
    // Not left for whoever gets the index next.
    const Entity next = world.create();
    const Entity other = world.create();
    REQUIRE_FALSE(world.contains<Spawner>(next));
    REQUIRE_FALSE(world.contains<Spawner>(other));
  }

  SECTION("A Recorded Component That Is Not Given Is Gone") {
    const auto alive = std::make_shared<int>(0);
    // Too long to be kept in the string itself.
    const std::string name(100, 'n');
    bool fails = false;
    scheduler.addParallel([&](Entity entity, Commands commands, const Position &) {
      commands.emplace<Named>(entity, name, alive);
      // The entity has one by then.
      commands.emplace<Named>(entity, name, alive);
    });
    scheduler.add([&](Query<const Health>) {
      if (fails) {
        throw std::runtime_error("failed");
      }
    });
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
    scheduler.run(world);
    REQUIRE(alive.use_count() == 1 + static_cast<long>(entities.size()));
    for (const Entity entity: entities) {
      REQUIRE(world.get<Named>(entity).name == name);
    }

    // Many more than fit into the memory kept for them, none of them given.
    for (int run = 0; run < 3; ++run) {
      scheduler.run(world);
      REQUIRE(alive.use_count() == 1 + static_cast<long>(entities.size()));
    }

    // Nor are the ones of a stage that failed.
    for (const Entity entity: entities) {
      world.erase<Named>(entity);
    }
    REQUIRE(alive.use_count() == 1);
    fails = true;
    REQUIRE_THROWS_AS(scheduler.run(world), std::runtime_error);
    REQUIRE(alive.use_count() == 1);
    REQUIRE_FALSE(world.contains<Named>(entities[0]));
  }

  SECTION("A Recorded Component That Cannot Be Made Ends The Run") {
    const auto alive = std::make_shared<int>(0);
    int ran = 0;
    scheduler.add([&](Commands commands) {
      commands.emplace<Spawner>(entities[0], 1);
      commands.emplace<Fragile>(entities[0], 2);
      commands.emplace<Named>(entities[0], "after", alive);
      commands.destroy(entities[1]);
    });
    scheduler.add([&](World &) { ++ran; });

    Fragile::breaks = true;
    REQUIRE_THROWS_AS(scheduler.run(world), std::runtime_error);
    Fragile::breaks = false;
    // What came before it is done, the rest is dropped.
    REQUIRE(ran == 0);
    REQUIRE(world.get<Spawner>(entities[0]).count == 1);
    REQUIRE_FALSE(world.contains<Fragile>(entities[0]));
    REQUIRE_FALSE(world.contains<Named>(entities[0]));
    REQUIRE(alive.use_count() == 1);
    REQUIRE(world.valid(entities[1]));

    // And nothing of it is left over for the next run.
    world.erase<Spawner>(entities[0]);
    scheduler.run(world);
    REQUIRE(ran == 1);
    REQUIRE(world.get<Fragile>(entities[0]).value == 2);
    REQUIRE(alive.use_count() == 2);
    REQUIRE_FALSE(world.valid(entities[1]));
  }

  SECTION("Recorded Changes Of Values Are Done In Their Order") {
    const auto alive = std::make_shared<int>(0);
    world.emplace<Spawner>(entities[0], 1);
    const Entity gone = entities[2];
    world.emplace<Spawner>(gone, 1);
    Entity made = sc::ecs2::null;
    int called = 0;
    scheduler.add([&](Commands commands) {
      // In the order recorded with the other changes of the component.
      commands.modify(entities[0], [](Spawner &spawner) { spawner.count *= 10; });
      commands.erase<Spawner>(entities[0]);
      commands.emplace<Spawner>(entities[0], 2);
      commands.modify(entities[0], [](Spawner &spawner) { spawner.count += 1; });
      made = commands.create();
      commands.emplace<Spawner>(made, 4);
      commands.modify(made, [](Spawner &spawner) { spawner.count += 1; });
      // Nothing for an entity without the component, or one that is no more,
      // or none at all.
      commands.modify(entities[1], [&](Spawner &) { ++called; });
      commands.modify(gone, [&](Spawner &) { ++called; });
      commands.modify(sc::ecs2::null, [&, alive](Spawner &) { ++called; });
      commands.emplace<Named>(sc::ecs2::null, "nobody", alive);
    });
    world.destroy(gone);
    scheduler.run(world);

    REQUIRE(world.get<Spawner>(entities[0]).count == 3);
    REQUIRE(world.get<Spawner>(made).count == 5);
    REQUIRE(called == 0);
    REQUIRE(alive.use_count() == 1);
  }

  SECTION("Recorded Changes Of Many Entities Keep Their Order Per Entity") {
    // Done on several threads, a page each, with the erases of a page
    // gathered: whatever comes after an erase of the same entity must see it
    // done, whatever comes before must not.
    const auto alive = std::make_shared<int>(0);
    scheduler.addParallel([&](Entity entity, Commands commands, const Position &pos) {
      const auto i = static_cast<size_t>(pos.x);
      if (i % 97 == 0) {
        // Nobody's: only dropped, also what the function holds.
        commands.emplace<Named>(sc::ecs2::null, "nobody", alive);
        commands.modify(sc::ecs2::null, [alive](Health &health) { health.value = -2; });
      }
      switch (i % 5) {
        case 0:
          commands.erase<Health>(entity);
          commands.emplace<Health>(entity, 7);
          commands.modify(entity, [](Health &health) { health.value += 1; });
          break;
        case 1:
          commands.erase<Health>(entity);
          break;
        case 2:
          // Has one: dropped, and the modify sees the old one.
          commands.emplace<Health>(entity, 5);
          commands.modify(entity, [](Health &health) { health.value -= 1; });
          break;
        case 3:
          commands.modify(entity, [](Health &health) { health.value *= 2; });
          commands.erase<Health>(entity);
          commands.emplace<Named>(entity, "named", alive);
          commands.erase<Named>(entity);
          break;
        default:
          commands.emplace<Named>(entity, "kept", alive);
          commands.destroy(entity);
          // Still done: entities go after all changes.
          commands.modify(entity, [](Health &health) { health.value = -1; });
          break;
      }
    });
    scheduler.run(world);

    size_t healthy = 0;
    for (size_t i = 0; i < entities.size(); ++i) {
      const Entity entity = entities[i];
      switch (i % 5) {
        case 0:
          REQUIRE(world.get<Health>(entity).value == 8);
          ++healthy;
          break;
        case 1:
        case 3:
          REQUIRE_FALSE(world.contains<Health>(entity));
          REQUIRE_FALSE(world.contains<Named>(entity));
          break;
        case 2:
          REQUIRE(world.get<Health>(entity).value == 99);
          ++healthy;
          break;
        default:
          REQUIRE_FALSE(world.valid(entity));
          break;
      }
    }
    // The counts of the pages agree with what is there, and what was owned
    // is given back.
    size_t counted = 0;
    world.each<const Health>([&](const Health &) { ++counted; });
    REQUIRE(counted == healthy);
    REQUIRE(world.column<Health>().page(0).count + world.column<Health>().page(1).count +
                world.column<Health>().page(2).count ==
            healthy);
    REQUIRE(alive.use_count() == 1);
    REQUIRE(world.column<Named>().pages() == world.column<Named>().spares());
  }

  SECTION("Threads Record Changes For The Same Components, Done Once The Stage Is Over") {
    // Every entity hits one of a few, as do many on other threads, and
    // every hit counts.
    constexpr size_t targets = 10;
    scheduler.addParallel([&](Commands commands, const Position &pos) {
      commands.modify(entities[static_cast<size_t>(pos.x) % targets], [](Health &health) { health.value -= 1; });
    });
    // Runs at the same time, as recording does not count as writing, and
    // sees the values as they were.
    std::atomic<size_t> changed = 0;
    scheduler.addParallel([&](const Health &health) { changed += health.value != 100; });
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
    scheduler.run(world);
    REQUIRE(changed == 0);
    scheduler.run(world);
    REQUIRE(changed == targets);

    for (size_t i = 0; i < entities.size(); ++i) {
      // How many entities hit it, twice each.
      const int hits = i < targets ? static_cast<int>((entities.size() - i + targets - 1) / targets) : 0;
      REQUIRE(world.get<Health>(entities[i]).value == 100 - (2 * hits));
    }
  }

  SECTION("A Recorded Change That Throws Ends The Run") {
    const auto alive = std::make_shared<int>(0);
    int ran = 0;
    bool throws = true;
    scheduler.add([&](Commands commands) {
      commands.modify(entities[0], [](Health &health) { health.value = 1; });
      commands.modify(entities[0], [&throws](Health &health) {
        health.value += 1;
        if (throws) {
          throw std::runtime_error("modify");
        }
      });
      commands.modify(entities[0], [alive](Health &health) { health.value += 10; });
      commands.destroy(entities[1]);
    });
    scheduler.add([&](World &) { ++ran; });

    REQUIRE_THROWS_AS(scheduler.run(world), std::runtime_error);
    // What came before it is done, also what it did itself, the rest is
    // dropped along with what it holds.
    REQUIRE(ran == 0);
    REQUIRE(world.get<Health>(entities[0]).value == 2);
    REQUIRE(alive.use_count() == 1);
    REQUIRE(world.valid(entities[1]));

    // And nothing of it is left over for the next run.
    throws = false;
    scheduler.run(world);
    REQUIRE(ran == 1);
    REQUIRE(world.get<Health>(entities[0]).value == 12);
    REQUIRE(alive.use_count() == 1);
    REQUIRE_FALSE(world.valid(entities[1]));
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
    for (const Entity entity: entities) {
      REQUIRE(world.valid(entity));
    }
  }
}
