#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

#include "sc/ecs/commands.hpp"
#include "sc/ecs/registry.hpp"
#include "sc/ecs/scheduler.hpp"
#include "sc/ecs/view.hpp"
#include "sc/threading/threading.hpp"

namespace {

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

  struct Frozen {};

  struct MissingEverywhere {
    int value{0};
  };

  struct Lifetime {
    int runs{0};
  };

  using Stages = std::vector<std::vector<size_t>>;

  void move(View<Position, const Velocity> view) {
    for (auto [entity, pos, vel] : view.each()) {
      pos.x += vel.dx;
      pos.y += vel.dy;
    }
  }

  struct Doubler {
    void operator()(const View<Position> &view) const {
      for (auto [entity, pos] : view.each()) {
        pos.x *= 2.0f;
        pos.y *= 2.0f;
      }
    }
  };

  // Waits until `arrived` reaches the expected count. False if it took so long
  // that the other systems are evidently not running at the same time.
  bool wait_for(const std::atomic<int> &arrived, int expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (arrived.load() < expected) {
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      std::this_thread::yield();
    }
    return true;
  }

} // namespace

TEST_CASE("Scheduler Stages", "[ecs][scheduler]") {
  Scheduler scheduler;

  SECTION("Nothing To Run") { REQUIRE(scheduler.stages().empty()); }

  SECTION("Readers Of The Same Component Run Together") {
    scheduler.add([](View<const Position>) {});
    scheduler.add([](View<const Position, const Velocity>) {});
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
  }

  SECTION("Writers Of Different Components Run Together") {
    scheduler.add([](View<Position>) {});
    scheduler.add([](View<Velocity>) {});
    scheduler.add([](View<Health>) {});
    REQUIRE(scheduler.stages() == Stages{{0, 1, 2}});
  }

  SECTION("A Writer Waits For Earlier Readers And Writers") {
    scheduler.add([](View<const Position>) {});
    scheduler.add([](View<Position>) {});
    scheduler.add([](View<Position>) {});
    scheduler.add([](View<const Position>) {});
    REQUIRE(scheduler.stages() == Stages{{0}, {1}, {2}, {3}});
  }

  SECTION("A System Only Waits For The Ones It Conflicts With") {
    scheduler.add([](View<Position, const Velocity>) {});
    scheduler.add([](View<Health>) {});
    scheduler.add([](View<const Position>) {});
    scheduler.add([](View<const Health>, View<const Velocity>) {});
    REQUIRE(scheduler.stages() == Stages{{0, 1}, {2, 3}});
  }

  SECTION("The Same Component In Several Parameters") {
    scheduler.add([](View<const Position>, View<Position, const Velocity>) {});
    scheduler.add([](View<const Position>) {});
    scheduler.add([](View<const Velocity>) {});
    REQUIRE(scheduler.stages() == Stages{{0, 2}, {1}});
  }

  SECTION("An Excluded Component Counts As Read") {
    using Unfrozen = decltype(std::declval<Registry &>().view<const Position>(entt::exclude<Frozen>));
    scheduler.add([](Unfrozen) {});
    scheduler.add([](View<Frozen>) {});
    scheduler.add([](View<const Frozen>) {});
    REQUIRE(scheduler.stages() == Stages{{0}, {1}, {2}});
  }

  SECTION("A System That Takes The Registry Runs Alone") {
    scheduler.add([](View<Position>) {});
    scheduler.add([](View<Health>) {});
    scheduler.add([](Registry &) {});
    scheduler.add([](Registry &) {});
    scheduler.add([](View<Position>) {});
    scheduler.add([](View<Health>) {});
    REQUIRE(scheduler.stages() == Stages{{0, 1}, {2}, {3}, {4, 5}});
  }

  SECTION("A System Without Parameters Runs Alone") {
    scheduler.add([](View<Position>) {});
    scheduler.add([] {});
    scheduler.add([](View<Health>) {});
    REQUIRE(scheduler.stages() == Stages{{0}, {1}, {2}});
  }

  SECTION("Systems Added Later Are Scheduled Too") {
    scheduler.add([](View<Position>) {});
    REQUIRE(scheduler.stages() == Stages{{0}});
    scheduler.add([](View<Health>) {}, "health");
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
    REQUIRE(scheduler.size() == 2);
    REQUIRE(scheduler.name(1) == "health");
  }
}

TEST_CASE("Scheduler Runs Systems", "[ecs][scheduler]") {
  sc::threading::init();

  Registry registry;
  std::vector<sc::ecs::Entity> entities(1000);
  registry.create(entities.begin(), entities.end());
  for (size_t i = 0; i < entities.size(); ++i) {
    registry.emplace<Position>(entities[i], static_cast<float>(i), 0.0f);
    registry.emplace<Velocity>(entities[i], 1.0f, 2.0f);
    registry.emplace<Health>(entities[i]);
  }

  Scheduler scheduler;

  SECTION("Functions, Function Objects And Lambdas") {
    scheduler.add(&move, "move");
    scheduler.add(Doubler{}, "double");
    scheduler.add([](View<Health> view) {
      for (auto [entity, health] : view.each()) {
        health.value -= 1;
      }
    });
    REQUIRE(scheduler.stages() == Stages{{0, 2}, {1}});

    scheduler.run(registry);
    scheduler.run(registry);

    for (size_t i = 0; i < entities.size(); ++i) {
      // Moved before doubled, in the order the systems were added.
      const float x = ((static_cast<float>(i) + 1.0f) * 2.0f + 1.0f) * 2.0f;
      REQUIRE(registry.get<Position>(entities[i]).x == x);
      REQUIRE(registry.get<Position>(entities[i]).y == 12.0f);
      REQUIRE(registry.get<Health>(entities[i]).value == 98);
    }
  }

  SECTION("A System With State") {
    int runs = 0;
    scheduler.add([&runs](View<const Position> view) { runs += static_cast<int>(view.size()); });
    scheduler.run(registry);
    scheduler.run(registry);
    REQUIRE(runs == 2000);
  }

  SECTION("A System That Takes The Registry May Change Its Structure") {
    scheduler.add([](View<Health> view) {
      for (auto [entity, health] : view.each()) {
        health.value = 0;
      }
    });
    scheduler.add([](Registry &reg) {
      std::vector<entt::entity> dead;
      for (auto [entity, health] : reg.view<const Health>().each()) {
        if (health.value == 0) {
          dead.push_back(entity);
        }
      }
      reg.destroy(dead.begin(), dead.end());
      reg.emplace<Health>(reg.create(), 7);
    });
    scheduler.add([](View<const Health> view) {
      REQUIRE(view.size() == 1);
      REQUIRE(view.get<const Health>(view.front()).value == 7);
    });

    scheduler.run(registry);
    REQUIRE(registry.view<Position>().size() == 0);
  }

  SECTION("The Systems Of A Stage Run At The Same Time") {
    if (std::thread::hardware_concurrency() < 4) {
      SKIP("needs at least three pool threads");
    }

    std::atomic<int> arrived{0};
    std::atomic<int> met{0};
    const auto meet = [&] {
      arrived.fetch_add(1);
      if (wait_for(arrived, 3)) {
        met.fetch_add(1);
      }
    };
    scheduler.add([meet](View<Position>) { meet(); });
    scheduler.add([meet](View<Velocity>) { meet(); });
    scheduler.add([meet](View<Health>) { meet(); });
    REQUIRE(scheduler.stages() == Stages{{0, 1, 2}});

    scheduler.run(registry);
    REQUIRE(met.load() == 3);
  }

  SECTION("A System That Is The Body Of A Loop") {
    for (size_t i = 0; i < entities.size(); i += 3) {
      registry.erase<Velocity>(entities[i]);
    }
    scheduler.addParallel([](Position &pos, const Velocity &vel) { pos.y += vel.dy; }, "move", 16);
    // Taken by value, a component is only read.
    scheduler.addParallel([](Health &health, Velocity vel) { health.value += static_cast<int>(vel.dy); }, "heal", 16);
    scheduler.addParallel([](const Velocity &) {});
    // Nothing to go through.
    scheduler.addParallel([](MissingEverywhere &) {});
    scheduler.addParallel([](Velocity &vel) { vel.dy += 1.0f; });
    // Towards the others, each of them is a system with a view of its components.
    REQUIRE(scheduler.stages() == Stages{{0, 1, 2, 3}, {4}});
    REQUIRE(scheduler.name(0) == "move");
    scheduler.run(registry);
    scheduler.run(registry);

    for (size_t i = 0; i < entities.size(); ++i) {
      const bool moving = i % 3 != 0;
      REQUIRE(registry.get<Position>(entities[i]).y == (moving ? 5.0f : 0.0f));
      REQUIRE(registry.get<Health>(entities[i]).value == (moving ? 105 : 100));
    }
  }

  SECTION("A Loop Over A Named View Or Group") {
    for (size_t i = 0; i < entities.size(); ++i) {
      if (i % 3 == 0) {
        registry.erase<Velocity>(entities[i]);
      }
      if (i % 5 == 0) {
        registry.emplace<Frozen>(entities[i]);
      }
    }
    scheduler.addParallel<View<Position, const Velocity, sc::ecs::Exclude<const Frozen>>>(
        [](Position &pos, const Velocity &vel) { pos.y += vel.dy; }, "move", 16);
    // Owns a component without members, which the function does not get.
    scheduler.addParallel<sc::ecs::Group<entt::owned_t<Health, const Frozen>, entt::get_t<const Position>>>(
        [](Health &health, const Position &) { health.value += 1; }, "heal", 16);
    // Owns nothing.
    scheduler.addParallel<
        sc::ecs::Group<entt::owned_t<>, entt::get_t<Velocity, const Position>, entt::exclude_t<Frozen>>>(
        [](Velocity &vel, const Position &) { vel.dx += 1.0f; }, "push", 16);
    // The registry has no entity for this group.
    scheduler.addParallel<sc::ecs::Group<entt::owned_t<MissingEverywhere>>>([](MissingEverywhere &) {});
    REQUIRE(scheduler.stages() == Stages{{0, 3}, {1, 2}});
    scheduler.run(registry);
    scheduler.run(registry);

    for (size_t i = 0; i < entities.size(); ++i) {
      const bool moving = i % 3 != 0 && i % 5 != 0;
      REQUIRE(registry.get<Position>(entities[i]).y == (moving ? 4.0f : 0.0f));
      REQUIRE(registry.get<Health>(entities[i]).value == (i % 5 == 0 ? 102 : 100));
      if (i % 3 != 0) {
        REQUIRE(registry.get<Velocity>(entities[i]).dx == (moving ? 3.0f : 1.0f));
      }
    }
  }

  SECTION("Going Through A View With Excluded And Empty Components") {
    // Not every entity is in the view: some lack a component, some have the
    // excluded one.
    for (size_t i = 0; i < entities.size(); ++i) {
      if (i % 3 == 0) {
        registry.erase<Velocity>(entities[i]);
      }
      if (i % 5 == 0) {
        registry.emplace<Frozen>(entities[i]);
      }
    }
    using Moving = View<Position, const Velocity, sc::ecs::Exclude<const Frozen>>;
    STATIC_REQUIRE(std::is_same_v<Moving, entt::view<entt::get_t<Position, const Velocity>,
                                                     entt::exclude_t<const Frozen>>>);
    // The registry makes the same view.
    size_t thawed = 0;
    for ([[maybe_unused]] const auto entity : registry.view<const Position, sc::ecs::Exclude<Frozen>>()) {
      ++thawed;
    }
    REQUIRE(thawed == entities.size() - (entities.size() + 4) / 5);
    scheduler.add([](Moving view) {
      sc::ecs::parallelEach(view, [](Position &pos, const Velocity &vel) { pos.y += vel.dy; }, 7);
    });
    // The smallest storage is the one of a component without members.
    scheduler.add([](View<const Frozen, Position> view) {
      sc::ecs::parallelEach(view, [](Position &pos) { pos.x += 100.0f; }, 3);
    });
    scheduler.run(registry);
    scheduler.run(registry);

    for (size_t i = 0; i < entities.size(); ++i) {
      const bool moving = i % 3 != 0 && i % 5 != 0;
      REQUIRE(registry.get<Position>(entities[i]).y == (moving ? 4.0f : 0.0f));
      REQUIRE(registry.get<Position>(entities[i]).x == static_cast<float>(i) + (i % 5 == 0 ? 200.0f : 0.0f));
    }
  }

  SECTION("Systems That Go Through A View On Several Threads") {
    for (size_t i = 0; i < entities.size(); i += 3) {
      registry.erase<Velocity>(entities[i]);
    }
    // Next to each other, both of them asking the pool for help.
    scheduler.add([](View<Position, const Velocity> view) {
      sc::ecs::parallelEach(view, [](Position &pos, const Velocity &vel) { pos.y += vel.dy; }, 16);
    });
    scheduler.add(
        [](View<Health> view) { sc::ecs::parallelEach(view, [](Health &health) { health.value += 1; }, 16); });
    scheduler.add(
        [](View<const MissingEverywhere> view) { sc::ecs::parallelEach(view, [](const MissingEverywhere &) {}); });
    REQUIRE(scheduler.stages() == Stages{{0, 1, 2}});
    for (int run = 0; run < 20; ++run) {
      scheduler.run(registry);
    }

    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(registry.get<Position>(entities[i]).y == (i % 3 != 0 ? 40.0f : 0.0f));
      REQUIRE(registry.get<Health>(entities[i]).value == 120);
    }
  }

  SECTION("An Exception Ends The Run After Its Stage") {
    std::atomic<bool> sibling{false};
    bool later = false;
    scheduler.add([](View<Position>) { throw std::runtime_error("broken system"); });
    scheduler.add([&sibling](View<Health>) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      sibling.store(true);
    });
    scheduler.add([&later](Registry &) { later = true; });

    REQUIRE_THROWS_AS(scheduler.run(registry), std::runtime_error);
    REQUIRE(sibling.load());
    REQUIRE(!later);
  }
}

TEST_CASE("Scheduler Runs Systems Reliably", "[ecs][scheduler][threading]") {
  sc::threading::init();

  constexpr int runs = 300;

  Registry registry;
  std::vector<sc::ecs::Entity> entities(1000);
  registry.create(entities.begin(), entities.end());
  for (size_t i = 0; i < entities.size(); ++i) {
    registry.emplace<Position>(entities[i], 0.0f, 0.0f);
    registry.emplace<Velocity>(entities[i], 1.0f, 2.0f);
    registry.emplace<Health>(entities[i]);
  }

  Scheduler scheduler;

  SECTION("Every System Runs Once In Every Run") {
    constexpr size_t system_count = 40;
    // Catch2 assertions are not made from pool threads.
    std::vector<std::atomic<int>> calls(system_count);
    for (size_t i = 0; i < system_count; ++i) {
      std::atomic<int> &mine = calls[i];
      switch (i % 5) {
      case 0:
        scheduler.add([&mine](View<Position>) { mine.fetch_add(1); });
        break;
      case 1:
        scheduler.add([&mine](View<const Position, const Velocity>) { mine.fetch_add(1); });
        break;
      case 2:
        scheduler.add([&mine](View<Velocity>) { mine.fetch_add(1); });
        break;
      case 3:
        scheduler.add([&mine](View<Health>) { mine.fetch_add(1); });
        break;
      default:
        scheduler.add([&mine](Registry &) { mine.fetch_add(1); });
        break;
      }
    }

    size_t scheduled = 0;
    for (const auto &stage : scheduler.stages()) {
      scheduled += stage.size();
    }
    REQUIRE(scheduled == system_count);

    for (int run = 1; run <= runs; ++run) {
      scheduler.run(registry);
    }
    for (size_t i = 0; i < system_count; ++i) {
      REQUIRE(calls[i].load() == runs);
    }
  }

  SECTION("Systems That Conflict Keep Their Order In Every Run") {
    // Only written by the systems that write Position, which never run at the
    // same time.
    std::vector<int> order;
    std::atomic<int> readers{0};
    for (int i = 0; i < 6; ++i) {
      scheduler.add([&order, i](View<Position>) { order.push_back(i); });
      // Something to run next to them.
      scheduler.add([&readers](View<const Velocity>) { readers.fetch_add(1); });
      scheduler.add([&readers](View<const Health>) { readers.fetch_add(1); });
    }

    bool inOrder = true;
    for (int run = 0; run < runs; ++run) {
      order.clear();
      scheduler.run(registry);
      inOrder = inOrder && order == std::vector<int>{0, 1, 2, 3, 4, 5};
    }
    REQUIRE(inOrder);
    REQUIRE(readers.load() == runs * 12);
  }

  SECTION("Readers Never See A Write In Progress") {
    std::atomic<size_t> torn{0};
    // Writes every entity, on several threads.
    scheduler.addParallel([](Position &pos) { pos.x += 1.0f; }, "first writer", 16);
    for (int reader = 0; reader < 6; ++reader) {
      scheduler.add([&torn](View<const Position> view) {
        const float expected = std::get<1>(*view.each().begin()).x;
        for (auto [entity, pos] : view.each()) {
          // Half of what the second writer adds would show here.
          if (pos.x != expected || pos.y != expected - 1.0f) {
            torn.fetch_add(1);
          }
        }
      });
    }
    scheduler.addParallel([](Position &pos) { pos.y = pos.x; }, "second writer", 16);
    REQUIRE(scheduler.stages() == Stages{{0}, {1, 2, 3, 4, 5, 6}, {7}});

    for (int run = 0; run < runs; ++run) {
      scheduler.run(registry);
    }
    REQUIRE(torn.load() == 0);
    for (const auto &e : entities) {
      REQUIRE(registry.get<Position>(e).x == static_cast<float>(runs));
      REQUIRE(registry.get<Position>(e).y == static_cast<float>(runs));
    }
  }

  SECTION("A Parallel Loop Visits Every Entity Once, Whatever Its Share") {
    // Some entities are not part of the loop.
    for (size_t i = 0; i < entities.size(); i += 7) {
      registry.erase<Velocity>(entities[i]);
    }
    // One entity per share, shares that do not divide the count, one share
    // for all.
    const std::vector<size_t> grains{1, 7, 64, 512, 100'000};
    for (const size_t grain : grains) {
      scheduler.addParallel([](Health &health, const Velocity &) { health.value += 1; }, {}, grain);
    }
    for (int run = 0; run < 50; ++run) {
      scheduler.run(registry);
    }

    const int visits = 50 * static_cast<int>(grains.size());
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(registry.get<Health>(entities[i]).value == 100 + (i % 7 != 0 ? visits : 0));
    }
  }

  SECTION("Works Again After An Exception From A Parallel Loop") {
    bool fail = false;
    int completed = 0;
    // Thrown on several threads at once; one of the exceptions arrives.
    scheduler.addParallel(
        [&fail](const Health &) {
          if (fail) {
            throw std::runtime_error("broken loop");
          }
        },
        {}, 16);
    scheduler.addParallel([](Position &pos) { pos.x += 1.0f; }, {}, 16);
    scheduler.add([&completed](Registry &) { ++completed; });

    for (int run = 0; run < 60; ++run) {
      fail = run % 3 == 0;
      if (fail) {
        REQUIRE_THROWS_AS(scheduler.run(registry), std::runtime_error);
      } else {
        REQUIRE_NOTHROW(scheduler.run(registry));
      }
    }
    // The stages after the broken one were skipped, nothing else.
    REQUIRE(completed == 40);
    // The loop next to the broken one still went through all of its entities.
    for (const auto &e : entities) {
      REQUIRE(registry.get<Position>(e).x == 60.0f);
    }
  }

  SECTION("Entities Come And Go Over Many Runs") {
    constexpr size_t spawned = 50;
    constexpr int lifetime = 3;
    // One without entities, so that every number comes from the runs below.
    Registry world;

    scheduler.addParallel([](Lifetime &age) { age.runs += 1; }, "age", 8);
    scheduler.add(
        [](View<const Lifetime> view, sc::ecs::Commands<> commands) {
          for (auto [entity, age] : view.each()) {
            if (age.runs >= lifetime) {
              commands.destroy(entity);
            }
          }
        },
        "reap");
    scheduler.add(
        [](sc::ecs::Commands<Lifetime, Position> commands) {
          for (size_t i = 0; i < spawned; ++i) {
            const auto e = commands.create();
            commands.emplace<Lifetime>(e);
            commands.emplace<Position>(e, static_cast<float>(i), 0.0f);
          }
        },
        "spawn");
    REQUIRE(scheduler.stages() == Stages{{0, 2}, {1}});

    for (int run = 1; run <= runs; ++run) {
      scheduler.run(world);
      const size_t alive = spawned * static_cast<size_t>(std::min(run, lifetime));
      // Checked every run, so that a wrong count shows where it starts.
      if (world.view<Lifetime>().size() != alive || world.view<Position>().size() != alive) {
        FAIL("wrong number of entities after run " << run);
      }
    }

    // The numbers of the destroyed entities are handed out again: after all
    // these runs, no entity has a number much higher than there ever were
    // entities at one time.
    const size_t bound = spawned * (lifetime + 1) + 2 * sc::ecs::internal::CommandBufferBase::ENTITY_BLOCK_SIZE;
    for (auto [entity, age, pos] : world.view<const Lifetime, const Position>().each()) {
      REQUIRE(world.valid(entity));
      REQUIRE(age.runs < lifetime);
      REQUIRE(entt::to_entity(entity) < bound);
    }
  }
}
