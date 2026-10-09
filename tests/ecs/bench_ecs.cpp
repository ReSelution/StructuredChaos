#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "entt/entt.hpp"

#include "sc/ecs/commands.hpp"
#include "sc/ecs/entity.hpp"
#include "sc/ecs/registry.hpp"
#include "sc/ecs/scheduler.hpp"
#include "sc/ecs/view.hpp"
#include "sc/threading/threading.hpp"

// Hidden by the [!benchmark] tag. Run with
//   meson test --benchmark -C <builddir>
// or directly with
//   bench_ecs "[!benchmark]"
// Use an optimized build without assertions (-Dbuildtype=release
// -Db_ndebug=true). Every benchmark does the same work on sc::ecs ("sc") and on
// a bare entt::registry ("entt").

namespace {

  constexpr size_t ENTITY_COUNT = 100'000;
  // Entities every spawning system creates per frame.
  constexpr size_t SPAWN_COUNT = 1'000;
  constexpr size_t SPAWNER_COUNT = 4;
  constexpr float DT = 1.0f / 60.0f;

  struct Position {
    float x{0.0f};
    float y{0.0f};
  };

  struct Velocity {
    float dx{0.0f};
    float dy{0.0f};
  };

  struct Health {
    float value{50.0f};
  };

  struct Timer {
    float elapsed{0.0f};
  };

  struct Energy {
    float value{0.0f};
  };

  struct Score {
    float value{0.0f};
  };

  struct Lifetime {
    uint32_t frames{1};
  };

  // The work of the systems, the same for every library.
  inline void integrate(Position &pos, const Velocity &vel) {
    pos.x += vel.dx * DT;
    pos.y += vel.dy * DT;
  }
  inline void regenerate(Health &health, const Velocity &vel) {
    health.value = std::min(100.0f, health.value + vel.dx * 0.1f);
  }
  inline void tick(Timer &timer, const Velocity &vel) { timer.elapsed += vel.dy * DT; }
  inline void charge(Energy &energy, const Velocity &vel) { energy.value += vel.dx * vel.dx + vel.dy * vel.dy; }
  inline void rate(Score &score, const Position &pos, const Health &health) {
    score.value = pos.x * 0.5f + health.value;
  }

  // One of several components that entities have in varying combinations.
  template <size_t Index> struct Extra {
    float value{0.0f};
  };
  constexpr size_t EXTRA_COUNT = 12;

  // Which Extra components the entity with the given number has, one bit for
  // each: one of 2^bits combinations, spread evenly and without a pattern.
  constexpr uint32_t combination(size_t i, unsigned bits) {
    return static_cast<uint32_t>((i * 2654435761u) >> 16) & ((1u << bits) - 1u);
  }

  // Calls the function with every Extra component of the combination.
  template <typename Function> void for_each_extra(uint32_t combination, Function &&function) {
    [&]<size_t... Index>(std::index_sequence<Index...>) {
      ((((combination >> Index) & 1u) != 0 ? function(Extra<Index>{}) : void()), ...);
    }(std::make_index_sequence<EXTRA_COUNT>{});
  }

  // Position and Velocity for every entity, plus its combination of Extra
  // components. Works for sc::ecs::Registry and entt::registry.
  template <typename Registry, typename Entities>
  void fill_fragmented(Registry &registry, const Entities &entities, unsigned bits) {
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      registry.template emplace<Position>(entities[i], static_cast<float>(i), 1.0f);
      registry.template emplace<Velocity>(entities[i], 1.0f, 2.0f);
      for_each_extra(combination(i, bits),
                     [&]<typename Component>(Component) { registry.template emplace<Component>(entities[i]); });
    }
  }

  // The order in which single entities are looked up.
  std::vector<size_t> shuffled_indices() {
    std::vector<size_t> indices(ENTITY_COUNT);
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      indices[i] = i;
    }
    std::shuffle(indices.begin(), indices.end(), std::mt19937{42});
    return indices;
  }

  // One fresh world per run of a sample, built outside the timing.
  template <typename World> std::vector<std::unique_ptr<World>> make_worlds(int count) {
    std::vector<std::unique_ptr<World>> worlds;
    worlds.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
      worlds.push_back(std::make_unique<World>());
    }
    return worlds;
  }

  struct ScWorld {
    sc::ecs::Registry registry;
    std::vector<sc::ecs::Entity> entities{ENTITY_COUNT};

    ScWorld() { registry.create(entities.begin(), entities.end()); }

    void fill() {
      for (size_t i = 0; i < ENTITY_COUNT; ++i) {
        registry.emplace<Position>(entities[i], static_cast<float>(i), 1.0f);
        registry.emplace<Velocity>(entities[i], 1.0f, 2.0f);
        registry.emplace<Health>(entities[i]);
        registry.emplace<Timer>(entities[i]);
        registry.emplace<Energy>(entities[i]);
        registry.emplace<Score>(entities[i]);
      }
    }
  };

  struct EnttWorld {
    entt::registry registry;
    std::vector<entt::entity> entities{ENTITY_COUNT};

    EnttWorld() { registry.create(entities.begin(), entities.end()); }

    void fill() {
      for (size_t i = 0; i < ENTITY_COUNT; ++i) {
        registry.emplace<Position>(entities[i], static_cast<float>(i), 1.0f);
        registry.emplace<Velocity>(entities[i], 1.0f, 2.0f);
        registry.emplace<Health>(entities[i]);
        registry.emplace<Timer>(entities[i]);
        registry.emplace<Energy>(entities[i]);
        registry.emplace<Score>(entities[i]);
      }
    }
  };

} // namespace

TEST_CASE("ECS Benchmarks: Entities", "[ecs][!benchmark]") {
  BENCHMARK_ADVANCED("sc: create one by one")
  (Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<sc::ecs::Registry>(meter.runs());
    meter.measure([&](int run) {
      auto &registry = *worlds[static_cast<size_t>(run)];
      sc::ecs::Entity last;
      for (size_t i = 0; i < ENTITY_COUNT; ++i) {
        last = registry.create();
      }
      return last.entity;
    });
  };

  BENCHMARK_ADVANCED("entt: create one by one")
  (Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<entt::registry>(meter.runs());
    meter.measure([&](int run) {
      auto &registry = *worlds[static_cast<size_t>(run)];
      entt::entity last{entt::null};
      for (size_t i = 0; i < ENTITY_COUNT; ++i) {
        last = registry.create();
      }
      return last;
    });
  };
}

TEST_CASE("ECS Benchmarks: Components", "[ecs][!benchmark]") {
  const std::vector<size_t> order = shuffled_indices();

  {
    ScWorld sc;
    sc.registry.reserve<Position>(ENTITY_COUNT);
    BENCHMARK("sc: emplace + erase") {
      for (const auto &e : sc.entities) {
        sc.registry.emplace<Position>(e, 1.0f, 2.0f);
      }
      for (const auto &e : sc.entities) {
        sc.registry.erase<Position>(e);
      }
    };

    sc.fill();
    BENCHMARK("sc: get 1 component") {
      float sum = 0.0f;
      for (const size_t i : order) {
        sum += sc.registry.get<const Position>(sc.entities[i]).x;
      }
      return sum;
    };
    BENCHMARK("sc: get 2 components") {
      float sum = 0.0f;
      for (const size_t i : order) {
        auto [pos, vel] = sc.registry.get<const Position, const Velocity>(sc.entities[i]);
        sum += pos.x + vel.dx;
      }
      return sum;
    };
  }

  {
    EnttWorld entt;
    entt.registry.storage<Position>().reserve(ENTITY_COUNT);
    BENCHMARK("entt: emplace + erase") {
      for (const auto e : entt.entities) {
        entt.registry.emplace<Position>(e, 1.0f, 2.0f);
      }
      for (const auto e : entt.entities) {
        entt.registry.erase<Position>(e);
      }
    };

    entt.fill();
    BENCHMARK("entt: get 1 component") {
      float sum = 0.0f;
      for (const size_t i : order) {
        sum += entt.registry.get<const Position>(entt.entities[i]).x;
      }
      return sum;
    };
    BENCHMARK("entt: get 2 components") {
      float sum = 0.0f;
      for (const size_t i : order) {
        auto [pos, vel] = entt.registry.get<const Position, const Velocity>(entt.entities[i]);
        sum += pos.x + vel.dx;
      }
      return sum;
    };
  }
}

TEST_CASE("ECS Benchmarks: Iteration", "[ecs][!benchmark]") {
  {
    ScWorld sc;
    sc.fill();
    BENCHMARK("sc: view, 2 components") {
      for (auto [entity, pos, vel] : sc.registry.view<Position, const Velocity>().each()) {
        integrate(pos, vel);
      }
    };
    BENCHMARK("sc: view, 3 components") {
      for (auto [entity, score, pos, health] : sc.registry.view<Score, const Position, const Health>().each()) {
        rate(score, pos, health);
      }
    };
    // Last: the group reorders the storages the views above went through.
    auto group = sc.registry.group<Position, Velocity>();
    BENCHMARK("sc: group, 2 components") {
      for (auto [entity, pos, vel] : group.each()) {
        integrate(pos, vel);
      }
    };
  }

  {
    EnttWorld entt;
    entt.fill();
    BENCHMARK("entt: view, 2 components") {
      for (auto [entity, pos, vel] : entt.registry.view<Position, const Velocity>().each()) {
        integrate(pos, vel);
      }
    };
    BENCHMARK("entt: view, 3 components") {
      for (auto [entity, score, pos, health] : entt.registry.view<Score, const Position, const Health>().each()) {
        rate(score, pos, health);
      }
    };
    auto group = entt.registry.group<Position, Velocity>();
    BENCHMARK("entt: group, 2 components") {
      for (auto [entity, pos, vel] : group.each()) {
        integrate(pos, vel);
      }
    };
  }
}

// Five systems over all entities. Four of them write different components and
// can run at the same time, the fifth reads what two of them wrote.
TEST_CASE("ECS Benchmarks: Frame", "[ecs][threading][!benchmark]") {
  sc::threading::init();

  {
    ScWorld sc;
    sc.fill();
    sc::ecs::Scheduler scheduler;
    scheduler.add([](sc::ecs::View<Position, const Velocity> view) {
      view.each([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    });
    scheduler.add([](sc::ecs::View<Health, const Velocity> view) {
      view.each([](Health &health, const Velocity &vel) { regenerate(health, vel); });
    });
    scheduler.add([](sc::ecs::View<Timer, const Velocity> view) {
      view.each([](Timer &timer, const Velocity &vel) { tick(timer, vel); });
    });
    scheduler.add([](sc::ecs::View<Energy, const Velocity> view) {
      view.each([](Energy &energy, const Velocity &vel) { charge(energy, vel); });
    });
    scheduler.add([](sc::ecs::View<Score, const Position, const Health> view) {
      view.each([](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); });
    });
    REQUIRE(scheduler.stages().size() == 2);
    BENCHMARK("sc: 5 systems, scheduler") { scheduler.run(sc.registry); };

    // The same systems, every one of them the body of a loop that asks the
    // thread pool for help, so that the threads go where work is left.
    {
      sc::ecs::Scheduler parallel;
      parallel.addParallel([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
      parallel.addParallel([](Health &health, const Velocity &vel) { regenerate(health, vel); });
      parallel.addParallel([](Timer &timer, const Velocity &vel) { tick(timer, vel); });
      parallel.addParallel([](Energy &energy, const Velocity &vel) { charge(energy, vel); });
      parallel.addParallel([](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); });
      REQUIRE(parallel.stages().size() == 2);
      BENCHMARK("sc: 5 systems, addParallel") { parallel.run(sc.registry); };
    }

    // What it costs to hand a stage to the thread pool and wait for it.
    sc::ecs::Scheduler idle;
    idle.add([](sc::ecs::View<Position>) {});
    idle.add([](sc::ecs::View<Health>) {});
    idle.add([](sc::ecs::View<Timer>) {});
    idle.add([](sc::ecs::View<Energy>) {});
    REQUIRE(idle.stages().size() == 1);
    BENCHMARK("sc: 4 empty systems, scheduler") { idle.run(sc.registry); };
  }

  {
    EnttWorld entt;
    entt.fill();
    BENCHMARK("entt: 5 systems, 1 thread") {
      entt.registry.view<Position, const Velocity>().each(
          [](Position &pos, const Velocity &vel) { integrate(pos, vel); });
      entt.registry.view<Health, const Velocity>().each(
          [](Health &health, const Velocity &vel) { regenerate(health, vel); });
      entt.registry.view<Timer, const Velocity>().each([](Timer &timer, const Velocity &vel) { tick(timer, vel); });
      entt.registry.view<Energy, const Velocity>().each(
          [](Energy &energy, const Velocity &vel) { charge(energy, vel); });
      entt.registry.view<Score, const Position, const Health>().each(
          [](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); });
    };
  }
}

// One system that moves all entities, on one thread and on all of them, with
// a view and with a group.
TEST_CASE("ECS Benchmarks: Parallel Loop", "[ecs][threading][!benchmark]") {
  sc::threading::init();
  using Moving = sc::ecs::Group<entt::owned_t<Position, const Velocity>>;

  ScWorld sc;
  sc.fill();
  {
    sc::ecs::Scheduler serial;
    serial.add([](sc::ecs::View<Position, const Velocity> view) {
      view.each([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    });
    BENCHMARK("sc: view, 1 thread") { serial.run(sc.registry); };

    sc::ecs::Scheduler parallel;
    parallel.addParallel([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    BENCHMARK("sc: view, addParallel") { parallel.run(sc.registry); };
  }
  // Last: the group reorders the storages the views above went through.
  {
    sc::ecs::Scheduler serial;
    serial.add([](Moving group) { group.each([](Position &pos, const Velocity &vel) { integrate(pos, vel); }); });
    BENCHMARK("sc: group, 1 thread") { serial.run(sc.registry); };

    sc::ecs::Scheduler parallel;
    parallel.addParallel<Moving>([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    BENCHMARK("sc: group, addParallel") { parallel.run(sc.registry); };
  }
}

// Every frame, four systems create entities with three components each and
// another destroys the entities of the frame before.
TEST_CASE("ECS Benchmarks: Frame With Structural Changes", "[ecs][threading][!benchmark]") {
  sc::threading::init();

  {
    sc::ecs::Registry registry;
    sc::ecs::Scheduler scheduler;
    scheduler.add([](sc::ecs::View<const Lifetime> view, sc::ecs::Commands<> commands) {
      for (auto entity : view) {
        commands.destroy(entity);
      }
    });
    for (size_t spawner = 0; spawner < SPAWNER_COUNT; ++spawner) {
      scheduler.add([](sc::ecs::Commands<Position, Velocity, Lifetime> commands) {
        std::array<sc::ecs::Entity, SPAWN_COUNT> entities;
        commands.create(entities.begin(), entities.end());
        for (size_t i = 0; i < SPAWN_COUNT; ++i) {
          commands.emplace<Position>(entities[i], static_cast<float>(i), 1.0f);
          commands.emplace<Velocity>(entities[i], 1.0f, 2.0f);
          commands.emplace<Lifetime>(entities[i]);
        }
      });
    }
    REQUIRE(scheduler.stages().size() == 1);
    BENCHMARK("sc: spawn + destroy, commands") { scheduler.run(registry); };
    REQUIRE(registry.view<Lifetime>().size() == SPAWNER_COUNT * SPAWN_COUNT);
  }

  // The same systems with the registry itself instead of Commands: they change
  // the storages directly, as the EnTT loop does, and run one after the other
  // on a single thread.
  {
    sc::ecs::Registry registry;
    sc::ecs::Scheduler scheduler;
    scheduler.add([](sc::ecs::Registry &registry) {
      auto old = registry.view<Lifetime>();
      registry.destroy(old.begin(), old.end());
    });
    scheduler.add([](sc::ecs::Registry &registry) {
      std::array<sc::ecs::Entity, SPAWNER_COUNT * SPAWN_COUNT> entities;
      registry.create(entities.begin(), entities.end());
      for (size_t i = 0; i < entities.size(); ++i) {
        registry.emplace<Position>(entities[i], static_cast<float>(i % SPAWN_COUNT), 1.0f);
        registry.emplace<Velocity>(entities[i], 1.0f, 2.0f);
        registry.emplace<Lifetime>(entities[i]);
      }
    });
    REQUIRE(scheduler.stages().size() == 2);
    BENCHMARK("sc: spawn + destroy, registry") { scheduler.run(registry); };
    REQUIRE(registry.view<Lifetime>().size() == SPAWNER_COUNT * SPAWN_COUNT);
  }

  {
    entt::registry registry;
    BENCHMARK("entt: spawn + destroy, 1 thread") {
      auto old = registry.view<Lifetime>();
      registry.destroy(old.begin(), old.end());
      std::array<entt::entity, SPAWNER_COUNT * SPAWN_COUNT> entities;
      registry.create(entities.begin(), entities.end());
      for (size_t i = 0; i < entities.size(); ++i) {
        registry.emplace<Position>(entities[i], static_cast<float>(i % SPAWN_COUNT), 1.0f);
        registry.emplace<Velocity>(entities[i], 1.0f, 2.0f);
        registry.emplace<Lifetime>(entities[i]);
      }
    };
    REQUIRE(registry.view<Lifetime>().size() == SPAWNER_COUNT * SPAWN_COUNT);
  }
}

// Every entity has Position and Velocity, and on top of that one of many
// combinations of other components. The loop only uses the two.
TEST_CASE("ECS Benchmarks: Fragmentation", "[ecs][!benchmark]") {
  for (const unsigned bits : {5u, 8u, 12u}) {
    const std::string suffix = ", " + std::to_string(1u << bits) + " combinations";

    {
      ScWorld sc;
      fill_fragmented(sc.registry, sc.entities, bits);
      BENCHMARK("sc: view" + suffix) {
        for (auto [entity, pos, vel] : sc.registry.view<Position, const Velocity>().each()) {
          integrate(pos, vel);
        }
      };
      auto group = sc.registry.group<Position, Velocity>();
      BENCHMARK("sc: group" + suffix) {
        for (auto [entity, pos, vel] : group.each()) {
          integrate(pos, vel);
        }
      };
    }

    {
      EnttWorld entt;
      fill_fragmented(entt.registry, entt.entities, bits);
      BENCHMARK("entt: view" + suffix) {
        for (auto [entity, pos, vel] : entt.registry.view<Position, const Velocity>().each()) {
          integrate(pos, vel);
        }
      };
      auto group = entt.registry.group<Position, Velocity>();
      BENCHMARK("entt: group" + suffix) {
        for (auto [entity, pos, vel] : group.each()) {
          integrate(pos, vel);
        }
      };
    }
  }

  // The storages of the two components in a different order, as it happens
  // over time when entities come and go.
  {
    const std::vector<size_t> order = shuffled_indices();
    EnttWorld entt;
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      entt.registry.emplace<Position>(entt.entities[i], static_cast<float>(i), 1.0f);
      entt.registry.emplace<Velocity>(entt.entities[order[i]], 1.0f, 2.0f);
    }
    BENCHMARK("entt: view, storages in different order") {
      for (auto [entity, pos, vel] : entt.registry.view<Position, const Velocity>().each()) {
        integrate(pos, vel);
      }
    };
    auto group = entt.registry.group<Position, Velocity>();
    BENCHMARK("entt: group, storages in different order") {
      for (auto [entity, pos, vel] : group.each()) {
        integrate(pos, vel);
      }
    };
  }
}
