#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <vector>

#include "entt/entt.hpp"
#include "sc/ecs/entity.hpp"
#include "sc/ecs/registry.hpp"
#include "sc/threading/threading.hpp"

// Hidden by the [!benchmark] tag, so that a plain test run stays fast. Run with
//   meson test --benchmark -C <builddir>
// or directly with
//   test_registry "[!benchmark]"
// Use an optimized build. Every benchmark works on 10,000 entities; the "entt"
// entries measure the same work on a bare entt::registry to show what the
// locking of sc::ecs::Registry costs.

namespace {

constexpr size_t ENTITY_COUNT = 10'000;

struct BenchPosition {
  float x{0.0f};
  float y{0.0f};
};

struct BenchVelocity {
  float dx{0.0f};
  float dy{0.0f};
};

struct BenchHealth {
  int value{100};
};

struct BenchTeam {
  int id{0};
};

// One fresh registry per run of a sample, built outside the timing. Only used
// for entity creation, where the registries hold no components.
template <typename Registry>
std::vector<std::unique_ptr<Registry>> make_registries(int count) {
  std::vector<std::unique_ptr<Registry>> registries;
  registries.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    registries.push_back(std::make_unique<Registry>());
  }
  return registries;
}

struct ScWorld {
  sc::ecs::Registry registry;
  std::vector<sc::ecs::Entity> entities{ENTITY_COUNT};

  ScWorld() { registry.create(entities.begin(), entities.end()); }

  void fill() {
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      registry.emplace<BenchPosition>(entities[i], static_cast<float>(i), 1.0f);
      registry.emplace<BenchVelocity>(entities[i], 1.0f, 2.0f);
    }
  }
};

struct EnttWorld {
  entt::registry registry;
  std::vector<entt::entity> entities{ENTITY_COUNT};

  EnttWorld() { registry.create(entities.begin(), entities.end()); }

  void fill() {
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      registry.emplace<BenchPosition>(entities[i], static_cast<float>(i), 1.0f);
      registry.emplace<BenchVelocity>(entities[i], 1.0f, 2.0f);
    }
  }
};

} // namespace

TEST_CASE("Registry Benchmarks: Entity Creation",
          "[ecs][registry][!benchmark]") {
  BENCHMARK_ADVANCED("sc: create one by one")
  (Catch::Benchmark::Chronometer meter) {
    auto registries = make_registries<sc::ecs::Registry>(meter.runs());
    meter.measure([&](int run) {
      auto &registry = *registries[static_cast<size_t>(run)];
      sc::ecs::Entity last;
      for (size_t i = 0; i < ENTITY_COUNT; ++i) {
        last = registry.create();
      }
      return last.entity;
    });
  };

  BENCHMARK_ADVANCED("entt: create one by one")
  (Catch::Benchmark::Chronometer meter) {
    auto registries = make_registries<entt::registry>(meter.runs());
    meter.measure([&](int run) {
      auto &registry = *registries[static_cast<size_t>(run)];
      entt::entity last{entt::null};
      for (size_t i = 0; i < ENTITY_COUNT; ++i) {
        last = registry.create();
      }
      return last;
    });
  };

  BENCHMARK_ADVANCED("sc: create in bulk")
  (Catch::Benchmark::Chronometer meter) {
    auto registries = make_registries<sc::ecs::Registry>(meter.runs());
    std::vector<sc::ecs::Entity> entities(ENTITY_COUNT);
    meter.measure([&](int run) {
      registries[static_cast<size_t>(run)]->create(entities.begin(),
                                                   entities.end());
      return entities.back().entity;
    });
  };

  BENCHMARK_ADVANCED("entt: create in bulk")
  (Catch::Benchmark::Chronometer meter) {
    auto registries = make_registries<entt::registry>(meter.runs());
    std::vector<entt::entity> entities(ENTITY_COUNT);
    meter.measure([&](int run) {
      registries[static_cast<size_t>(run)]->create(entities.begin(),
                                                   entities.end());
      return entities.back();
    });
  };
}

// The write benchmarks restore the state they started from, so that they can
// run on one registry any number of times. A registry per run is not an
// option: every component of every registry holds a thread-local key, and
// a process only has about a thousand of them.
TEST_CASE("Registry Benchmarks: Component Writes",
          "[ecs][registry][!benchmark]") {
  ScWorld sc_world;
  EnttWorld entt_world;
  const std::vector<BenchPosition> data(ENTITY_COUNT);

  BENCHMARK("sc: emplace + erase") {
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      sc_world.registry.emplace<BenchPosition>(sc_world.entities[i],
                                               static_cast<float>(i), 1.0f);
    }
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      sc_world.registry.erase<BenchPosition>(sc_world.entities[i]);
    }
  };

  BENCHMARK("entt: emplace + erase") {
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      entt_world.registry.emplace<BenchPosition>(entt_world.entities[i],
                                                 static_cast<float>(i), 1.0f);
    }
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      entt_world.registry.erase<BenchPosition>(entt_world.entities[i]);
    }
  };

  BENCHMARK("sc: insert range + erase") {
    sc_world.registry.insert<BenchPosition>(
        sc_world.entities.begin(), sc_world.entities.end(), data.begin());
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      sc_world.registry.erase<BenchPosition>(sc_world.entities[i]);
    }
  };

  BENCHMARK("entt: insert range + erase") {
    entt_world.registry.insert<BenchPosition>(
        entt_world.entities.begin(), entt_world.entities.end(), data.begin());
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      entt_world.registry.erase<BenchPosition>(entt_world.entities[i]);
    }
  };
}

TEST_CASE("Registry Benchmarks: Component Reads",
          "[ecs][registry][!benchmark]") {
  ScWorld sc_world;
  sc_world.fill();
  EnttWorld entt_world;
  entt_world.fill();

  BENCHMARK("sc: get 1 component") {
    float sum = 0.0f;
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      auto [lock, position] =
          sc_world.registry.get<BenchPosition>(sc_world.entities[i]);
      sum += position.x;
    }
    return sum;
  };

  BENCHMARK("entt: get 1 component") {
    float sum = 0.0f;
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      sum += entt_world.registry.get<BenchPosition>(entt_world.entities[i]).x;
    }
    return sum;
  };

  BENCHMARK("sc: get 2 components") {
    float sum = 0.0f;
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      auto [lock, position, velocity] =
          sc_world.registry.get<BenchPosition, BenchVelocity>(
              sc_world.entities[i]);
      sum += position.x + velocity.dx;
    }
    return sum;
  };

  BENCHMARK("entt: get 2 components") {
    float sum = 0.0f;
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      auto [position, velocity] =
          entt_world.registry.get<BenchPosition, BenchVelocity>(
              entt_world.entities[i]);
      sum += position.x + velocity.dx;
    }
    return sum;
  };

  BENCHMARK("sc: iterate view, 2 components") {
    float sum = 0.0f;
    auto view = sc_world.registry.view<BenchPosition, BenchVelocity>();
    view.raw().each(
        [&sum](const BenchPosition &position, const BenchVelocity &velocity) {
          sum += position.x + velocity.dx;
        });
    return sum;
  };

  BENCHMARK("entt: iterate view, 2 components") {
    float sum = 0.0f;
    entt_world.registry.view<BenchPosition, BenchVelocity>().each(
        [&sum](const BenchPosition &position, const BenchVelocity &velocity) {
          sum += position.x + velocity.dx;
        });
    return sum;
  };
}

TEST_CASE("Registry Benchmarks: Parallel Reads",
          "[ecs][registry][threading][!benchmark]") {
  sc::threading::init();

  constexpr int TASK_COUNT = 10;

  ScWorld sc_world;
  sc_world.fill();

  // Every task reads two components of all entities. With get() a task takes
  // and releases both shared locks once per entity, with a view once in all.
  BENCHMARK("sc: get 2 components, 10 pool tasks") {
    std::vector<int> tasks(TASK_COUNT);
    sc::threading::detachBatch(
        std::move(tasks),
        [&](int thread_id, int task) {
          float sum = 0.0f;
          for (size_t i = 0; i < ENTITY_COUNT; ++i) {
            auto [lock, position, velocity] =
                sc_world.registry.get<BenchPosition, BenchVelocity>(
                    sc_world.entities[i]);
            sum += position.x + velocity.dx;
          }
          volatile float sink = sum;
          (void)sink;
        },
        nullptr);
    sc::threading::wait_until_finished();
  };

  BENCHMARK("sc: view, 2 components, 10 pool tasks") {
    std::vector<int> tasks(TASK_COUNT);
    sc::threading::detachBatch(
        std::move(tasks),
        [&](int thread_id, int task) {
          float sum = 0.0f;
          auto view = sc_world.registry.view<BenchPosition, BenchVelocity>();
          view.raw().each([&sum](const BenchPosition &position,
                                 const BenchVelocity &velocity) {
            sum += position.x + velocity.dx;
          });
          volatile float sink = sum;
          (void)sink;
        },
        nullptr);
    sc::threading::wait_until_finished();
  };
}

TEST_CASE("Registry Benchmarks: Parallel Writes",
          "[ecs][registry][threading][!benchmark]") {
  sc::threading::init();

  constexpr int BATCH_COUNT = 10;
  constexpr size_t BATCH_SIZE = ENTITY_COUNT / BATCH_COUNT;

  const auto reserve_all = [](sc::ecs::Registry &registry) {
    registry.reserve<BenchPosition>(ENTITY_COUNT);
    registry.reserve<BenchVelocity>(ENTITY_COUNT);
    registry.reserve<BenchHealth>(ENTITY_COUNT);
    registry.reserve<BenchTeam>(ENTITY_COUNT);
  };

  // Entities cannot be destroyed, so these two set up and tear down their
  // registry inside the timing. That is small next to the measured work and
  // the same for both.
  //
  // Every task creates its own entities and adds four components, so the
  // tasks contend for the entity mutex and the four component mutexes.
  BENCHMARK("sc: 4 components, 10 pool tasks") {
    sc::ecs::Registry registry;
    reserve_all(registry);

    std::vector<int> batches(BATCH_COUNT);
    sc::threading::detachBatch(
        std::move(batches),
        [&registry](int thread_id, int batch) {
          for (size_t i = 0; i < BATCH_SIZE; ++i) {
            auto entity = registry.create();
            entity.add<BenchPosition>();
            entity.add<BenchVelocity>();
            entity.add<BenchHealth>();
            entity.add<BenchTeam>(batch);
          }
        },
        nullptr);
    sc::threading::wait_until_finished();
  };

  BENCHMARK("sc: 4 components, 1 thread") {
    sc::ecs::Registry registry;
    reserve_all(registry);

    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      auto entity = registry.create();
      entity.add<BenchPosition>();
      entity.add<BenchVelocity>();
      entity.add<BenchHealth>();
      entity.add<BenchTeam>(0);
    }
  };

  // Entity creation alone, without any component.
  BENCHMARK("sc: create only, 10 pool tasks") {
    sc::ecs::Registry registry;

    std::vector<int> batches(BATCH_COUNT);
    sc::threading::detachBatch(
        std::move(batches),
        [&registry](int thread_id, int batch) {
          for (size_t i = 0; i < BATCH_SIZE; ++i) {
            registry.create();
          }
        },
        nullptr);
    sc::threading::wait_until_finished();
  };

  BENCHMARK("sc: create only, 1 thread") {
    sc::ecs::Registry registry;

    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      registry.create();
    }
  };

  // The same work as above, but written in batches: a task creates its
  // entities with one call and adds each component with one insert, so it
  // takes every mutex once instead of once per entity.
  const std::vector<BenchPosition> positions(BATCH_SIZE);
  const std::vector<BenchVelocity> velocities(BATCH_SIZE);
  const std::vector<BenchHealth> healths(BATCH_SIZE);
  const std::vector<BenchTeam> teams(BATCH_SIZE);

  const auto write_batch = [&](sc::ecs::Registry &registry) {
    std::vector<sc::ecs::Entity> entities(BATCH_SIZE);
    registry.create(entities.begin(), entities.end());
    registry.insert<BenchPosition>(entities.begin(), entities.end(),
                                   positions.begin());
    registry.insert<BenchVelocity>(entities.begin(), entities.end(),
                                   velocities.begin());
    registry.insert<BenchHealth>(entities.begin(), entities.end(),
                                 healths.begin());
    registry.insert<BenchTeam>(entities.begin(), entities.end(),
                               teams.begin());
  };

  BENCHMARK("sc: batched, 10 pool tasks") {
    sc::ecs::Registry registry;
    reserve_all(registry);

    std::vector<int> batches(BATCH_COUNT);
    sc::threading::detachBatch(
        std::move(batches),
        [&](int thread_id, int batch) { write_batch(registry); }, nullptr);
    sc::threading::wait_until_finished();
  };

  BENCHMARK("sc: batched, 1 thread") {
    sc::ecs::Registry registry;
    reserve_all(registry);

    for (int batch = 0; batch < BATCH_COUNT; ++batch) {
      write_batch(registry);
    }
  };

  BENCHMARK("entt: batched, 1 thread") {
    entt::registry registry;
    registry.storage<BenchPosition>().reserve(ENTITY_COUNT);
    registry.storage<BenchVelocity>().reserve(ENTITY_COUNT);
    registry.storage<BenchHealth>().reserve(ENTITY_COUNT);
    registry.storage<BenchTeam>().reserve(ENTITY_COUNT);

    for (int batch = 0; batch < BATCH_COUNT; ++batch) {
      std::vector<entt::entity> entities(BATCH_SIZE);
      registry.create(entities.begin(), entities.end());
      registry.insert<BenchPosition>(entities.begin(), entities.end(),
                                     positions.begin());
      registry.insert<BenchVelocity>(entities.begin(), entities.end(),
                                     velocities.begin());
      registry.insert<BenchHealth>(entities.begin(), entities.end(),
                                   healths.begin());
      registry.insert<BenchTeam>(entities.begin(), entities.end(),
                                 teams.begin());
    }
  };

  // The case the per-component locks are made for: every task writes a
  // component of its own, so no two tasks ever want the same component mutex.
  // Emplace and erase again, so that the world can be reused.
  ScWorld sc_world;
  EnttWorld entt_world;

  const auto round_trip = [](auto &world, auto component) {
    using Component = decltype(component);
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      world.registry.template emplace<Component>(world.entities[i]);
    }
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      world.registry.template erase<Component>(world.entities[i]);
    }
  };

  BENCHMARK("sc: 1 task per component") {
    sc::threading::detach(
        [&](int thread_id) { round_trip(sc_world, BenchPosition{}); });
    sc::threading::detach(
        [&](int thread_id) { round_trip(sc_world, BenchVelocity{}); });
    sc::threading::detach(
        [&](int thread_id) { round_trip(sc_world, BenchHealth{}); });
    sc::threading::detach(
        [&](int thread_id) { round_trip(sc_world, BenchTeam{}); });
    sc::threading::wait_until_finished();
  };

  BENCHMARK("sc: per component, 1 thread") {
    round_trip(sc_world, BenchPosition{});
    round_trip(sc_world, BenchVelocity{});
    round_trip(sc_world, BenchHealth{});
    round_trip(sc_world, BenchTeam{});
  };

  BENCHMARK("entt: per component, 1 thread") {
    round_trip(entt_world, BenchPosition{});
    round_trip(entt_world, BenchVelocity{});
    round_trip(entt_world, BenchHealth{});
    round_trip(entt_world, BenchTeam{});
  };
}
