#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <vector>

#include <mustache/ecs/ecs.hpp>

#include "common.hpp"

using namespace compare;

namespace {

  constexpr auto SERIAL = mustache::JobRunMode::kCurrentThread;
  constexpr auto PARALLEL = mustache::JobRunMode::kParallel;

  struct Target {
    mustache::Entity entity;
  };

  struct World {
    mustache::World world;
    std::vector<mustache::Entity> entities;

    World() = default;

    explicit World(size_t count, const mustache::WorldContext &context = {}) : world(context) {
      entities.reserve(count);
      for (size_t i = 0; i < count; ++i) {
        entities.push_back(world.entities().create());
      }
    }

    void fill_moving() {
      for (size_t i = 0; i < entities.size(); ++i) {
        world.entities().assign<Position>(entities[i], static_cast<float>(i), 1.0f);
        world.entities().assign<Velocity>(entities[i], 1.0f, 2.0f);
      }
    }

    void fill() {
      fill_moving();
      for (const mustache::Entity entity : entities) {
        world.entities().assign<Health>(entity, 50.0f);
        world.entities().assign<Timer>(entity, 0.0f);
        world.entities().assign<Energy>(entity, 0.0f);
        world.entities().assign<Score>(entity, 0.0f);
      }
    }

    // Entity i gets the entity order[i] as its target.
    void fill_targets(const std::vector<size_t> &order) {
      for (size_t i = 0; i < entities.size(); ++i) {
        world.entities().assign<Target>(entities[i], entities[order[i]]);
      }
    }

    void fill_fragmented() {
      fill_moving();
      for (size_t i = 0; i < entities.size(); ++i) {
        for_each_extra(combination(i),
                       [&]<typename Component>(Component) { world.entities().assign<Component>(entities[i], 0.0f); });
      }
    }
  };

} // namespace

// On all threads, a loop is a forEach with JobRunMode::kParallel. While it
// runs, mustache keeps what the threads change in the structure of the world
// in a storage per thread and makes the changes when the loop is done.
TEST_CASE(SMALL_WORLD, "[ecs_compare][!benchmark]") {
  // The calling thread works on the loops as well.
  const unsigned threads = thread_count();
  mustache::WorldContext context;
  context.dispatcher = std::make_shared<mustache::Dispatcher>(threads - 1);

  BENCHMARK_ADVANCED(name::serial(name::CREATE))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<World>(meter.runs());
    meter.measure([&](int run) {
      mustache::EntityManager &entities = worlds[static_cast<size_t>(run)]->world.entities();
      for (size_t i = 0; i < ENTITY_COUNT; ++i) {
        (void)entities.begin().assign<Position>(static_cast<float>(i), 1.0f).assign<Velocity>(1.0f, 2.0f).end();
      }
    });
  };

  {
    // One spawner per thread, each of which creates its part of the entities.
    // An entity created inside a loop gets its components without values.
    const auto prepare = [threads](World &world) {
      for (unsigned worker = 0; worker < threads; ++worker) {
        const Slice part = slice(ENTITY_COUNT, worker, threads);
        world.world.entities().assign<Spawner>(world.world.entities().create(), static_cast<uint32_t>(part.first),
                                               static_cast<uint32_t>(part.last));
      }
    };
    const auto spawn = [](World &world) {
      mustache::EntityManager &entities = world.world.entities();
      entities.forEach(
          [&entities](const Spawner &spawner) {
            for (uint32_t i = spawner.first; i < spawner.last; ++i) {
              (void)entities.create<Position, Velocity>();
            }
          },
          PARALLEL);
    };
    const auto count = [](World &world) {
      size_t result = 0;
      world.world.entities().forEach([&result](const Position &, const Velocity &) { ++result; }, SERIAL);
      return result;
    };
    {
      World world{0, context};
      prepare(world);
      spawn(world);
      REQUIRE(count(world) == ENTITY_COUNT);
    }
    BENCHMARK_ADVANCED(name::parallel(name::CREATE))(Catch::Benchmark::Chronometer meter) {
      auto worlds = make_worlds<World>(meter.runs(), size_t{0}, context);
      for (auto &world : worlds) {
        prepare(*world);
      }
      meter.measure([&](int run) { spawn(*worlds[static_cast<size_t>(run)]); });
    };
  }

  BENCHMARK_ADVANCED(name::serial(name::DESTROY))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<World>(meter.runs(), ENTITY_COUNT);
    for (auto &world : worlds) {
      world->fill_moving();
    }
    meter.measure([&](int run) {
      World &world = *worlds[static_cast<size_t>(run)];
      for (const mustache::Entity entity : world.entities) {
        world.world.entities().destroyNow(entity);
      }
    });
  };

  {
    const auto reap = [](World &world) {
      mustache::EntityManager &entities = world.world.entities();
      entities.forEach([&entities](mustache::Entity entity, const Position &) { entities.destroyNow(entity); },
                       PARALLEL);
    };
    {
      World world{ENTITY_COUNT, context};
      world.fill_moving();
      reap(world);
      size_t left = 0;
      world.world.entities().forEach([&left](const Position &) { ++left; }, SERIAL);
      REQUIRE(left == 0);
      REQUIRE_FALSE(world.world.entities().isEntityValid(world.entities.front()));
    }
    BENCHMARK_ADVANCED(name::parallel(name::DESTROY))(Catch::Benchmark::Chronometer meter) {
      auto worlds = make_worlds<World>(meter.runs(), ENTITY_COUNT, context);
      for (auto &world : worlds) {
        world->fill_moving();
      }
      meter.measure([&](int run) { reap(*worlds[static_cast<size_t>(run)]); });
    };
  }

  {
    World world{ENTITY_COUNT, context};
    world.fill_moving();
    mustache::EntityManager &entities = world.world.entities();
    BENCHMARK(name::serial(name::ADD_REMOVE)) {
      for (const mustache::Entity entity : world.entities) {
        entities.assign<Health>(entity, 50.0f);
      }
      for (const mustache::Entity entity : world.entities) {
        entities.removeComponent<Health>(entity);
      }
    };

    const auto add = [&entities] {
      entities.forEach(
          [&entities](mustache::Entity entity, const Position &) { entities.assign<Health>(entity, 50.0f); }, PARALLEL);
    };
    const auto remove = [&entities] {
      entities.forEach(
          [&entities](mustache::Entity entity, const Health &) { entities.removeComponent<Health>(entity); }, PARALLEL);
    };
    const auto healthy = [&entities] {
      size_t result = 0;
      entities.forEach([&result](const Health &) { ++result; }, SERIAL);
      return result;
    };
    add();
    REQUIRE(healthy() == ENTITY_COUNT);
    remove();
    REQUIRE(healthy() == 0);
    BENCHMARK(name::parallel(name::ADD_REMOVE)) {
      add();
      remove();
    };
    REQUIRE(healthy() == 0);
  }

  {
    const std::vector<size_t> order = shuffled_indices(ENTITY_COUNT);
    World world{ENTITY_COUNT, context};
    world.fill();
    mustache::EntityManager &entities = world.world.entities();
    BENCHMARK(name::serial(name::GET)) {
      float sum = 0.0f;
      for (const size_t i : order) {
        sum += entities.getComponent<const Position>(world.entities[i])->x;
      }
      return sum;
    };
    for (const auto mode : {SERIAL, PARALLEL}) {
      const auto named = [mode](const char *benchmark) {
        return mode == SERIAL ? name::serial(benchmark) : name::parallel(benchmark);
      };
      BENCHMARK(named(name::ITERATE_1)) {
        entities.forEach([](Position &pos) { nudge(pos); }, mode);
      };
      BENCHMARK(named(name::ITERATE_2)) {
        entities.forEach([](Position &pos, const Velocity &vel) { integrate(pos, vel); }, mode);
      };
      BENCHMARK(named(name::ITERATE_3)) {
        entities.forEach([](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); },
                         mode);
      };
    }
  }

  {
    World world{ENTITY_COUNT, context};
    world.fill_fragmented();
    for (const auto mode : {SERIAL, PARALLEL}) {
      BENCHMARK(mode == SERIAL ? name::serial(name::ITERATE_FRAGMENTED) : name::parallel(name::ITERATE_FRAGMENTED)) {
        world.world.entities().forEach([](Position &pos, const Velocity &vel) { integrate(pos, vel); }, mode);
      };
    }
  }
}

TEST_CASE(LARGE_WORLD, "[ecs_compare][!benchmark]") {
  // The calling thread works on the loops as well.
  mustache::WorldContext context;
  context.dispatcher = std::make_shared<mustache::Dispatcher>(thread_count() - 1);

  World world{PARALLEL_ENTITY_COUNT, context};
  world.fill();
  mustache::EntityManager &entities = world.world.entities();

  for (const auto mode : {SERIAL, PARALLEL}) {
    const auto named = [mode](const char *benchmark) {
      return mode == SERIAL ? name::serial(benchmark) : name::parallel(benchmark);
    };

    BENCHMARK(named(name::LOOP_LIGHT)) {
      entities.forEach([](Position &pos, const Velocity &vel) { integrate(pos, vel); }, mode);
    };

    BENCHMARK(named(name::LOOP_HEAVY)) {
      entities.forEach([](Position &pos, const Velocity &vel) { simulate(pos, vel); }, mode);
    };

    BENCHMARK(named(name::FRAME)) {
      entities.forEach([](Position &pos, const Velocity &vel) { integrate(pos, vel); }, mode);
      entities.forEach([](Health &health, const Velocity &vel) { regenerate(health, vel); }, mode);
      entities.forEach([](Timer &timer, const Velocity &vel) { tick(timer, vel); }, mode);
      entities.forEach([](Energy &energy, const Velocity &vel) { charge(energy, vel); }, mode);
      entities.forEach([](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); }, mode);
    };
  }
  REQUIRE(entities.getComponent<const Timer>(world.entities.back())->elapsed > 0.0f);

  // Only now, so that the benchmarks above run on the world they always had.
  const std::vector<size_t> order = shuffled_indices(PARALLEL_ENTITY_COUNT);
  world.fill_targets(order);
  for (const auto mode : {SERIAL, PARALLEL}) {
    BENCHMARK(mode == SERIAL ? name::serial(name::LOOP_LOOKUP) : name::parallel(name::LOOP_LOOKUP)) {
      entities.forEach(
          [&](Velocity &vel, const Position &pos, const Target &target) {
            steer(vel, pos, *entities.getComponent<const Position>(target.entity));
          },
          mode);
    };
  }
  const mustache::Entity last = world.entities.back();
  REQUIRE(steered(*entities.getComponent<const Velocity>(last), *entities.getComponent<const Position>(last),
                  *entities.getComponent<const Position>(world.entities[order.back()])));
}

namespace {

  struct Steady {
    using Handle = mustache::Entity;
    mustache::World world;
    mustache::EntityManager &entities = world.entities();

    Handle spawn(size_t number) {
      const mustache::Entity entity = entities.create();
      entities.assign<Position>(entity, static_cast<float>(number), 1.0f);
      entities.assign<Velocity>(entity, 1.0f, 2.0f);
      if (number % 2 == 0) {
        entities.assign<Health>(entity, 50.0f);
      }
      return entity;
    }
    void destroy(Handle entity) { entities.destroyNow(entity); }
    void add_timer(Handle entity) { entities.assign<Timer>(entity, 0.0f); }
    void remove_timer(Handle entity) { entities.removeComponent<Timer>(entity); }
    void frame() {
      entities.forEach([](Position &pos, const Velocity &vel) { integrate(pos, vel); }, SERIAL);
      entities.forEach([](Health &health, const Velocity &vel) { regenerate(health, vel); }, SERIAL);
      entities.forEach([](Timer &timer, const Velocity &vel) { tick(timer, vel); }, SERIAL);
    }
  };

} // namespace

TEST_CASE(STEADY_WORLD, "[ecs_compare][!benchmark]") {
  Steady steady;
  report(run_steady(steady));
  size_t count = 0;
  steady.entities.forEach([&count](const Position &) { ++count; }, SERIAL);
  REQUIRE(count == ENTITY_COUNT);
}

namespace {

  struct Heavy {
    using Handle = mustache::Entity;
    mustache::World world;
    mustache::EntityManager &entities = world.entities();

    Handle spawn() {
      const mustache::Entity entity = entities.create();
      entities.assign<Transform>(entity, Transform{});
      entities.assign<Body>(entity, Body{1.0f, {}});
      return entity;
    }
    void add_health(Handle entity) { entities.assign<Health>(entity, 50.0f); }
    void remove_health(Handle entity) { entities.removeComponent<Health>(entity); }
    void spin() {
      entities.forEach([](Transform &transform) { compare::spin(transform); }, SERIAL);
    }
    void weigh() {
      entities.forEach([](Body &body) { compare::weigh(body); }, SERIAL);
    }
    void drop() {
      entities.forEach([](Transform &transform, const Body &body) { compare::drop(transform, body); }, SERIAL);
    }
  };

} // namespace

TEST_CASE(HEAVY_WORLD, "[ecs_compare][!benchmark]") { run_heavy<Heavy>(); }

// The memory of the worlds, see memory in common.hpp.
TEST_CASE(memory::MOVING, "[ecs_compare][.memory]") {
  report_memory<World>(memory::MOVING, ENTITY_COUNT, &World::fill_moving);
}
TEST_CASE(memory::FULL, "[ecs_compare][.memory]") { report_memory<World>(memory::FULL, ENTITY_COUNT, &World::fill); }
TEST_CASE(memory::FRAGMENTED, "[ecs_compare][.memory]") {
  report_memory<World>(memory::FRAGMENTED, ENTITY_COUNT, &World::fill_fragmented);
}
TEST_CASE(memory::LARGE, "[ecs_compare][.memory]") {
  report_memory<World>(memory::LARGE, PARALLEL_ENTITY_COUNT, &World::fill);
}
TEST_CASE(memory::HEAVY, "[ecs_compare][.memory]") { report_heavy_memory<Heavy>(); }
TEST_CASE(memory::STEADY, "[ecs_compare][.memory]") { report_steady_memory<Steady>(); }
