#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
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

  // Stays where it is in its storage, which leaves gaps behind.
  struct Pinned {
    static constexpr auto in_place_delete = true;
    int visits{0};
  };

  // For the loops that are checked against what EnTT goes through: every
  // component carries the number of its entity, so that one that is handed
  // out for another entity shows.
  using Number = std::underlying_type_t<entt::entity>;
  struct Alpha {
    Number owner;
  };
  struct Beta {
    Number owner;
  };
  struct Gamma {
    Number owner;
  };
  // Its storage is in pieces that end elsewhere than those of the others.
  struct SmallPages {
    static constexpr size_t page_size = 64;
    Number owner;
  };
  struct Anchored {
    static constexpr auto in_place_delete = true;
    Number owner;
  };
  struct Tag {};

  // Goes through a view or group with each and parallelEach and requires that
  // both call the function once for exactly the entities EnTT goes through,
  // with the components of that entity.
  template<typename Source>
  void requireSameAsEntt(Source source, size_t grain) {
    INFO(entt::type_name<Source>::value());
    std::vector<entt::entity> expected;
    uint32_t highest = 0;
    for (const entt::entity entity: source) {
      expected.push_back(entity);
      highest = std::max(highest, static_cast<uint32_t>(entt::to_entity(entity)));
    }
    std::ranges::sort(expected);

    std::vector<entt::entity> seen;
    size_t wrong = 0;
    sc::ecs::each(source, [&](entt::entity entity, const auto &...components) {
      seen.push_back(entity);
      wrong += ((components.owner != entt::to_integral(entity)) || ... || false);
    });
    std::ranges::sort(seen);
    REQUIRE(wrong == 0);
    REQUIRE(seen == expected);

    std::vector<std::atomic<uint32_t>> visits(highest + 1);
    std::atomic<size_t> wrongOnThreads = 0;
    std::atomic<size_t> calls = 0;
    sc::ecs::parallelEach(
        source,
        [&](entt::entity entity, const auto &...components) {
          ++calls;
          if (entt::to_entity(entity) > highest || ((components.owner != entt::to_integral(entity)) || ... || false)) {
            ++wrongOnThreads;
          } else {
            ++visits[entt::to_entity(entity)];
          }
        },
        grain);
    REQUIRE(wrongOnThreads == 0);
    REQUIRE(calls == expected.size());
    size_t once = 0;
    for (const entt::entity entity: expected) {
      once += visits[entt::to_entity(entity)] == 1;
    }
    REQUIRE(once == expected.size());
  }

  using Stages = std::vector<std::vector<size_t>>;

  void move(View<Position, const Velocity> view) {
    for (auto [entity, pos, vel]: view.each()) {
      pos.x += vel.dx;
      pos.y += vel.dy;
    }
  }

  struct Doubler {
    void operator()(const View<Position> &view) const {
      for (auto [entity, pos]: view.each()) {
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
      for (auto [entity, health]: view.each()) {
        health.value -= 1;
      }
    });
    REQUIRE(scheduler.stages() == Stages{{0, 2}, {1}});

    scheduler.run(registry);
    scheduler.run(registry);

    for (size_t i = 0; i < entities.size(); ++i) {
      // Moved before doubled, in the order the systems were added.
      const float x = (((static_cast<float>(i) + 1.0f) * 2.0f) + 1.0f) * 2.0f;
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
      for (auto [entity, health]: view.each()) {
        health.value = 0;
      }
    });
    scheduler.add([](Registry &reg) {
      std::vector<entt::entity> dead;
      for (auto [entity, health]: reg.view<const Health>().each()) {
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
    scheduler
        .addParallel<sc::ecs::Group<entt::owned_t<>, entt::get_t<Velocity, const Position>, entt::exclude_t<Frozen>>>(
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
    STATIC_REQUIRE(
        std::is_same_v<Moving, entt::view<entt::get_t<Position, const Velocity>, entt::exclude_t<const Frozen>>>);
    // The registry makes the same view.
    size_t thawed = 0;
    for ([[maybe_unused]] const auto entity: registry.view<const Position, sc::ecs::Exclude<Frozen>>()) {
      ++thawed;
    }
    REQUIRE(thawed == entities.size() - ((entities.size() + 4) / 5));
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
  for (auto entitie: entities) {
    registry.emplace<Position>(entitie, 0.0f, 0.0f);
    registry.emplace<Velocity>(entitie, 1.0f, 2.0f);
    registry.emplace<Health>(entitie);
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
    for (const auto &stage: scheduler.stages()) {
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
        for (auto [entity, pos]: view.each()) {
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
    for (const auto &e: entities) {
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
    for (const size_t grain: grains) {
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
    for (const auto &e: entities) {
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
          for (auto [entity, age]: view.each()) {
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
    const size_t bound = (spawned * (lifetime + 1)) + (2 * sc::ecs::internal::CommandBufferBase::ENTITY_BLOCK_SIZE);
    for (auto [entity, age, pos]: world.view<const Lifetime, const Position>().each()) {
      REQUIRE(world.valid(entity));
      REQUIRE(age.runs < lifetime);
      REQUIRE(entt::to_entity(entity) < bound);
    }
  }
}

TEST_CASE("Loops Over Components That Fill Several Pages", "[ecs][scheduler]") {
  sc::threading::init();

  // More than the storages keep in one piece.
  constexpr size_t count = (5 * ENTT_PACKED_PAGE) - 120;

  Registry registry;
  std::vector<sc::ecs::Entity> entities(count);
  registry.create(entities.begin(), entities.end());
  for (size_t i = 0; i < count; ++i) {
    registry.emplace<Position>(entities[i], static_cast<float>(i), 0.0f);
    registry.emplace<Velocity>(entities[i], 1.0f, 2.0f);
    registry.emplace<Health>(entities[i]);
  }

  const auto visit = [](Position &pos, const Velocity &vel, Health &health) {
    pos.y += vel.dy;
    health.value += static_cast<int>(pos.x);
  };
  // Every entity that has all three components was visited once with its own
  // components, and no other one.
  const auto check = [&](int runs) {
    for (size_t i = 0; i < count; ++i) {
      const bool visited = registry.storage<Velocity>().contains(entities[i]);
      REQUIRE(registry.get<Position>(entities[i]).y == (visited ? 2.0f * static_cast<float>(runs) : 0.0f));
      REQUIRE(registry.get<Health>(entities[i]).value == 100 + (visited ? runs * static_cast<int>(i) : 0));
    }
  };

  SECTION("Storages With The Same Entities At The Same Positions") {
    sc::ecs::each(registry.view<Position, const Velocity, Health>(), visit);
    check(1);
    // Shares that end in the middle of a page.
    sc::ecs::parallelEach(registry.view<Position, const Velocity, Health>(), visit, 100);
    check(2);
  }

  SECTION("Storages That Differ From Some Position On") {
    // The last velocity takes the place of the erased one, in the third page,
    // and the storage ends earlier than the others.
    registry.erase<Velocity>(entities[(2 * ENTT_PACKED_PAGE) + 50]);
    sc::ecs::each(registry.view<Position, const Velocity, Health>(), visit);
    check(1);
    sc::ecs::parallelEach(registry.view<Position, const Velocity, Health>(), visit, 100);
    check(2);

    // In no page any more.
    for (size_t i = 0; i < count; i += 7) {
      if (registry.storage<Velocity>().contains(entities[i])) {
        registry.erase<Velocity>(entities[i]);
      }
    }
    for (size_t i = 0; i < count; ++i) {
      registry.get<Position>(entities[i]).y = 0.0f;
      registry.get<Health>(entities[i]).value = 100;
    }
    sc::ecs::each(registry.view<Position, const Velocity, Health>(), visit);
    check(1);
  }

  SECTION("A Group") {
    auto group = registry.group<Position, Velocity>(entt::get<Health>);
    sc::ecs::each(group, visit);
    check(1);
    registry.erase<Velocity>(entities[(2 * ENTT_PACKED_PAGE) + 50]);
    sc::ecs::each(group, visit);
    sc::ecs::parallelEach(group, visit, 100);
    REQUIRE(registry.get<Position>(entities[(2 * ENTT_PACKED_PAGE) + 50]).y == 2.0f);
    REQUIRE(registry.get<Position>(entities[0]).y == 6.0f);
    REQUIRE(registry.get<Position>(entities[count - 1]).y == 6.0f);
    REQUIRE(registry.get<Health>(entities[count - 1]).value == 100 + (3 * static_cast<int>(count - 1)));
  }

  SECTION("A Storage That Keeps Its Order") {
    for (size_t i = 0; i < count / 2; ++i) {
      registry.emplace<Pinned>(entities[i]);
    }
    for (size_t i = 0; i < count / 2; i += 3) {
      registry.erase<Pinned>(entities[i]);
    }
    const auto pin = [](Pinned &pinned, Position &pos) {
      ++pinned.visits;
      pos.y += 1.0f;
    };
    sc::ecs::each(registry.view<Pinned, Position>(), pin);
    sc::ecs::parallelEach(registry.view<Pinned, Position>(), pin, 100);
    sc::ecs::each(registry.view<Pinned>(), [](Pinned &pinned) { ++pinned.visits; });
    for (size_t i = 0; i < count; ++i) {
      const bool pinned = i < count / 2 && i % 3 != 0;
      REQUIRE(registry.get<Position>(entities[i]).y == (pinned ? 2.0f : 0.0f));
      if (pinned) {
        REQUIRE(registry.get<Pinned>(entities[i]).visits == 3);
      }
    }
  }
}

TEST_CASE("Loops That Take The Entity And Record Changes", "[ecs][scheduler][commands][threading]") {
  sc::threading::init();

  using sc::ecs::Commands;

  constexpr size_t count = (3 * ENTT_PACKED_PAGE) + 200;

  Registry registry;
  std::vector<sc::ecs::Entity> entities(count);
  registry.create(entities.begin(), entities.end());
  for (size_t i = 0; i < count; ++i) {
    registry.emplace<Position>(entities[i], static_cast<float>(i), 0.0f);
    registry.emplace<Velocity>(entities[i], 1.0f, 2.0f);
    // The number of the entity, to tell whether a function got the right one.
    registry.emplace<Health>(entities[i], static_cast<int>(entt::to_integral(entities[i].entity)));
  }

  Scheduler scheduler;

  SECTION("The Entity Comes Before The Components") {
    std::atomic<size_t> wrong = 0;
    const auto visit = [&](entt::entity entity, Position &pos, const Health &health) {
      wrong += std::cmp_not_equal(entt::to_integral(entity), health.value);
      pos.y += 1.0f;
    };
    sc::ecs::each(registry.view<Position, const Health>(), visit);
    sc::ecs::parallelEach(registry.view<Position, const Health>(), visit, 100);
    // Looked up, entity by entity.
    registry.erase<Health>(entities[ENTT_PACKED_PAGE + 3]);
    sc::ecs::each(registry.view<Position, const Health>(), visit);
    sc::ecs::each(registry.view<Position, const Health, sc::ecs::Exclude<Frozen>>(), visit);
    auto group = registry.group<Position, Health>();
    sc::ecs::each(group, visit);
    sc::ecs::parallelEach(group, visit, 100);

    REQUIRE(wrong == 0);
    REQUIRE(registry.get<Position>(entities[0]).y == 6.0f);
    REQUIRE(registry.get<Position>(entities[ENTT_PACKED_PAGE + 3]).y == 2.0f);
    REQUIRE(registry.get<Position>(entities[count - 1]).y == 6.0f);
  }

  SECTION("A Loop That Destroys, Adds And Removes") {
    scheduler.addParallel(
        [](entt::entity entity, Commands<Frozen, Velocity> commands, const Position &pos) {
          const auto i = static_cast<size_t>(pos.x);
          if (i % 3 == 0) {
            commands.destroy(entity);
          } else if (i % 3 == 1) {
            commands.emplace<Frozen>(entity);
          } else {
            commands.remove<Velocity>(entity);
          }
        },
        "sort out", 64);
    // Does not need the entity.
    scheduler.addParallel(
        [](Commands<Lifetime, MissingEverywhere> commands, const Health &) {
          const auto spawned = commands.create();
          commands.emplace<Lifetime>(spawned, 7);
          commands.emplace<MissingEverywhere>(spawned, 7);
        },
        "spawn", 64);
    // Only reads, and records changes: nothing keeps them apart.
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
    scheduler.run(registry);

    for (size_t i = 0; i < count; ++i) {
      REQUIRE(registry.valid(entities[i]) == (i % 3 != 0));
      if (i % 3 != 0) {
        REQUIRE(registry.storage<Frozen>().contains(entities[i]) == (i % 3 == 1));
        REQUIRE(registry.storage<Velocity>().contains(entities[i]) == (i % 3 == 1));
      }
    }
    // One for every entity there was, whatever thread recorded it.
    REQUIRE(registry.view<Lifetime>().size() == count);
    REQUIRE(registry.view<MissingEverywhere>().size() == count);
    size_t spawned = 0;
    sc::ecs::each(registry.view<const Lifetime, const MissingEverywhere>(),
                  [&](entt::entity entity, const Lifetime &lifetime, const MissingEverywhere &missing) {
                    spawned += registry.valid(entity) && lifetime.runs == 7 && missing.value == 7;
                  });
    REQUIRE(spawned == count);
    // Added in the same order by every thread, so they are side by side.
    const auto &lifetimes = registry.storage<Lifetime>();
    const auto &missing = registry.storage<MissingEverywhere>();
    REQUIRE(std::equal(lifetimes.data(), lifetimes.data() + count, missing.data()));

    // Nothing is left over for the next run.
    const size_t frozen = registry.view<Frozen>().size();
    Scheduler nothing;
    nothing.addParallel([](entt::entity, Commands<Frozen> &, const Position &) {});
    nothing.run(registry);
    nothing.run(registry);
    REQUIRE(registry.view<Frozen>().size() == frozen);
  }

  SECTION("A Loop Over A Named View Or Group That Records Changes") {
    for (size_t i = 0; i < count; i += 5) {
      registry.emplace<Frozen>(entities[i]);
    }
    scheduler.addParallel<View<const Position, sc::ecs::Exclude<Frozen>>>(
        [](entt::entity entity, Commands<Lifetime> commands, const Position &) { commands.emplace<Lifetime>(entity); },
        "age", 64);
    scheduler.addParallel<sc::ecs::Group<entt::owned_t<Health, const Velocity>>>(
        [](entt::entity entity, Commands<> commands, Health &health, const Velocity &) {
          if (health.value % 2 == 0) {
            commands.destroy(entity);
          }
        },
        "reap", 64);
    scheduler.run(registry);

    for (size_t i = 0; i < count; ++i) {
      const bool alive = entt::to_integral(entities[i].entity) % 2 != 0;
      REQUIRE(registry.valid(entities[i]) == alive);
      if (alive) {
        REQUIRE(registry.storage<Lifetime>().contains(entities[i]) == (i % 5 != 0));
      }
    }
  }

  SECTION("A System That Records Changes From Its Own Loops") {
    scheduler.add([](View<const Position> view, sc::ecs::ParallelCommands<Frozen> commands) {
      sc::ecs::parallelEach(
          view,
          [&](entt::entity entity, const Position &pos) {
            if (static_cast<size_t>(pos.x) % 2 == 0) {
              commands.local().emplace<Frozen>(entity);
            }
          },
          64);
      // On the thread of the system itself.
      auto local = commands.local();
      local.emplace<Frozen>(local.create());
    });
    scheduler.add([](View<const Frozen>, Commands<Frozen> commands) { commands.emplace<Frozen>(commands.create()); });
    for (int run = 1; run <= 3; ++run) {
      scheduler.run(registry);
      REQUIRE(registry.view<Frozen>().size() == ((count + 1) / 2) + 2);
      for (auto entity: registry.view<Frozen>()) {
        registry.erase<Frozen>(entity);
      }
    }
  }

  SECTION("Threads Outside The Pool Record Through ParallelCommands") {
    constexpr size_t threadCount = 8;
    scheduler.add([&](sc::ecs::ParallelCommands<Frozen> commands) {
      std::vector<std::jthread> threads;
      for (size_t t = 0; t < threadCount; ++t) {
        threads.emplace_back([&, t] {
          auto local = commands.local();
          // A second one and a copy on the same thread, which share the lock.
          auto again = commands.local();
          const auto copy = again;
          for (size_t i = t; i < count; i += threadCount) {
            (i % 2 == 0 ? local : again).emplace<Frozen>(entities[i]);
          }
          static_cast<void>(copy);
        });
      }
    });
    scheduler.run(registry);

    REQUIRE(registry.view<Frozen>().size() == count);
    for (size_t i = 0; i < count; ++i) {
      REQUIRE(registry.storage<Frozen>().contains(entities[i]));
    }
  }

  SECTION("An Exception Drops What Every Thread Recorded") {
    scheduler.addParallel(
        [](entt::entity entity, Commands<Frozen> commands, const Position &pos) {
          commands.emplace<Frozen>(entity);
          if (static_cast<size_t>(pos.x) == count - 1) {
            throw std::runtime_error("last");
          }
        },
        "freeze", 64);
    REQUIRE_THROWS_AS(scheduler.run(registry), std::runtime_error);
    REQUIRE(registry.view<Frozen>().size() == 0);
  }
}

TEST_CASE("Loops Go Through What EnTT Goes Through, Whatever Happened To The Storages", "[ecs][scheduler][threading]") {
  sc::threading::init();

  using sc::ecs::Exclude;

  const uint32_t seed = GENERATE(1u, 2u, 3u);
  std::mt19937 random{seed};
  const auto below = [&](size_t bound) { return std::uniform_int_distribution<size_t>{0, bound - 1}(random); };

  Registry registry;
  std::vector<entt::entity> alive;

  const auto owner = [](entt::entity entity) { return entt::to_integral(entity); };
  // Adds the component to an entity that does not have it, takes it away from
  // one that has it.
  const auto toggle = [&]<typename Component>(entt::entity entity) {
    if (registry.storage<Component>().contains(entity)) {
      registry.erase<Component>(entity);
    } else if constexpr (std::is_empty_v<Component>) {
      registry.emplace<Component>(entity);
    } else {
      registry.emplace<Component>(entity, owner(entity));
    }
  };
  const auto toggleAny = [&](entt::entity entity) {
    switch (below(6)) {
      case 0:
        toggle.template operator()<Alpha>(entity);
        break;
      case 1:
        toggle.template operator()<Beta>(entity);
        break;
      case 2:
        toggle.template operator()<Gamma>(entity);
        break;
      case 3:
        toggle.template operator()<SmallPages>(entity);
        break;
      case 4:
        toggle.template operator()<Anchored>(entity);
        break;
      default:
        toggle.template operator()<Tag>(entity);
        break;
    }
  };

  SECTION("Views") {
    // Everything side by side to begin with.
    for (size_t i = 0; i < (4 * ENTT_PACKED_PAGE) + 300; ++i) {
      const entt::entity entity = registry.create();
      alive.push_back(entity);
      registry.emplace<Alpha>(entity, owner(entity));
      registry.emplace<Beta>(entity, owner(entity));
      registry.emplace<Gamma>(entity, owner(entity));
      registry.emplace<SmallPages>(entity, owner(entity));
      registry.emplace<Anchored>(entity, owner(entity));
    }

    const auto check = [&] {
      const size_t grain = 1 + below(700);
      requireSameAsEntt(registry.view<Alpha>(), grain);
      requireSameAsEntt(registry.view<Alpha, const Beta>(), grain);
      requireSameAsEntt(registry.view<Beta, Alpha>(), grain);
      requireSameAsEntt(registry.view<Alpha, Beta, const Gamma>(), grain);
      requireSameAsEntt(registry.view<SmallPages>(), grain);
      requireSameAsEntt(registry.view<SmallPages, Alpha>(), grain);
      requireSameAsEntt(registry.view<Alpha, const SmallPages, Beta>(), grain);
      requireSameAsEntt(registry.view<Anchored>(), grain);
      requireSameAsEntt(registry.view<Anchored, Alpha>(), grain);
      requireSameAsEntt(registry.view<Alpha, const Anchored, Gamma>(), grain);
      requireSameAsEntt(registry.view<const Tag, Alpha>(), grain);
      requireSameAsEntt(registry.view<Alpha, Exclude<Beta>>(), grain);
      requireSameAsEntt(registry.view<Alpha, const Beta, Exclude<Tag>>(), grain);
    };
    check();

    for (int round = 0; round < 40; ++round) {
      CAPTURE(seed, round);
      // Few changes at first, which leave most pages as they are, then many.
      const size_t changes = round < 20 ? 1 + below(4) : 1 + below(alive.size() / 4);
      for (size_t change = 0; change < changes; ++change) {
        const size_t index = below(alive.size());
        switch (below(8)) {
          case 0:
            registry.destroy(alive[index]);
            alive[index] = alive.back();
            alive.pop_back();
            break;
          case 1: {
            // The number of a destroyed entity comes back with a new version.
            const entt::entity entity = registry.create();
            alive.push_back(entity);
            for (int component = 0; component < 4; ++component) {
              toggleAny(entity);
            }
            break;
          }
          default:
            toggleAny(alive[index]);
            break;
        }
      }

      // Reordered storages: by entity, by something else, like another one.
      // The entities of the storage, which itself goes through its components.
      const entt::sparse_set &alphas = registry.storage<Alpha>();
      switch (below(6)) {
        case 0:
          registry.storage<Alpha>().sort([](entt::entity lhs, entt::entity rhs) { return lhs < rhs; });
          registry.storage<Beta>().sort([](entt::entity lhs, entt::entity rhs) { return lhs < rhs; });
          registry.storage<Gamma>().sort([](entt::entity lhs, entt::entity rhs) { return lhs < rhs; });
          registry.storage<SmallPages>().sort([](entt::entity lhs, entt::entity rhs) { return lhs < rhs; });
          break;
        case 1: {
          const auto salt = static_cast<uint32_t>(random());
          registry.storage<Beta>().sort([salt](entt::entity lhs, entt::entity rhs) {
            return (entt::to_integral(lhs) ^ salt) * 2654435761u < (entt::to_integral(rhs) ^ salt) * 2654435761u;
          });
          break;
        }
        case 2:
          registry.storage<Beta>().sort_as(alphas.begin(), alphas.end());
          break;
        case 3:
          registry.storage<SmallPages>().sort_as(alphas.begin(), alphas.end());
          registry.storage<Gamma>().sort_as(alphas.begin(), alphas.end());
          break;
        default:
          break;
      }

      check();
    }
  }

  SECTION("Groups") {
    // Two that own their components, one of them only a part, and one that
    // owns nothing.
    auto pairs = registry.group<Alpha, Beta>();
    auto triples = registry.group<Gamma>(entt::get<const SmallPages, Alpha>);
    auto loose = registry.group<>(entt::get<SmallPages, Anchored>, entt::exclude<Tag>);

    for (size_t i = 0; i < (3 * ENTT_PACKED_PAGE) + 300; ++i) {
      const entt::entity entity = registry.create();
      alive.push_back(entity);
      for (int component = 0; component < 5; ++component) {
        toggleAny(entity);
      }
    }

    for (int round = 0; round < 30; ++round) {
      const size_t changes = 1 + below(round < 10 ? 4 : alive.size() / 4);
      for (size_t change = 0; change < changes; ++change) {
        toggleAny(alive[below(alive.size())]);
      }
      const size_t grain = 1 + below(700);
      requireSameAsEntt(pairs, grain);
      requireSameAsEntt(triples, grain);
      requireSameAsEntt(loose, grain);
      requireSameAsEntt(registry.view<Alpha, const Beta>(), grain);
      requireSameAsEntt(registry.view<Gamma, Alpha, SmallPages>(), grain);
    }
  }
}
