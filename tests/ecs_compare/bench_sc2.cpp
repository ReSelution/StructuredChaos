#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <utility>
#include <vector>

#include "common.hpp"
#include "sc/ecs2/scheduler.hpp"
#include "sc/ecs2/world.hpp"
#include "sc/threading/threading.hpp"

using namespace compare;

// On all threads, a loop is a system added with addParallel. Components are
// added and removed by the loop itself, which the scheduler treats as writing
// them; entities are destroyed through Commands, once the loop is over.

namespace {

  using sc::ecs2::Change;
  using sc::ecs2::Commands;
  using sc::ecs2::Entity;
  using sc::ecs2::Query;
  using sc::ecs2::Scheduler;

  // The executable is built several times, see meson.build: once for each
  // kind of pages a world can have its columns backed with.
#ifndef ECS_COMPARE_PAGES
#define ECS_COMPARE_PAGES Huge
#endif
  constexpr sc::ecs2::Pages PAGES = sc::ecs2::Pages::ECS_COMPARE_PAGES;

  struct Target {
    Entity entity;
  };

  struct World {
    sc::ecs2::World world{PAGES};
    std::vector<Entity> entities;

    explicit World(size_t count) : entities(count) {
      for (Entity &entity : entities) {
        entity = world.create();
      }
    }

    void fill_moving() {
      for (size_t i = 0; i < entities.size(); ++i) {
        world.emplace<Position>(entities[i], static_cast<float>(i), 1.0f);
        world.emplace<Velocity>(entities[i], 1.0f, 2.0f);
      }
    }

    void fill() {
      fill_moving();
      for (const Entity entity : entities) {
        world.emplace<Health>(entity, 50.0f);
        world.emplace<Timer>(entity, 0.0f);
        world.emplace<Energy>(entity, 0.0f);
        world.emplace<Score>(entity, 0.0f);
      }
    }

    // Entity i gets the entity order[i] as its target.
    void fill_targets(const std::vector<size_t> &order) {
      for (size_t i = 0; i < entities.size(); ++i) {
        world.emplace<Target>(entities[i], entities[order[i]]);
      }
    }

    void fill_fragmented() {
      fill_moving();
      for (size_t i = 0; i < entities.size(); ++i) {
        for_each_extra(combination(i),
                       [&]<typename Component>(Component) { world.emplace<Component>(entities[i], 0.0f); });
      }
    }
  };

  // All entities and their components in one call.
  template <typename First, typename Second>
  void create_all(sc::ecs2::World &world, size_t count, const First &first, const Second &second) {
    std::vector<Entity> entities(count);
    world.create(entities, first, second);
  }

} // namespace

TEST_CASE(SMALL_WORLD, "[ecs_compare][!benchmark]") {
  // The thread that starts a loop works on it as well.
  const size_t workers = thread_count();
  sc::threading::init(static_cast<uint32_t>(workers) - 1);

  BENCHMARK_ADVANCED(name::serial(name::CREATE))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<sc::ecs2::World>(meter.runs(), PAGES);
    meter.measure([&](int run) {
      auto &world = *worlds[static_cast<size_t>(run)];
      for (size_t i = 0; i < ENTITY_COUNT; ++i) {
        const Entity entity = world.create();
        world.emplace<Position>(entity, static_cast<float>(i), 1.0f);
        world.emplace<Velocity>(entity, 1.0f, 2.0f);
      }
    });
  };

  {
    sc::ecs2::World world{PAGES};
    create_all(world, ENTITY_COUNT, Position{0.0f, 1.0f}, Velocity{1.0f, 2.0f});
    size_t count = 0;
    world.each<const Position, const Velocity>(
        [&](const Position &pos, const Velocity &vel) { count += pos.y == 1.0f && vel.dy == 2.0f; });
    REQUIRE(count == ENTITY_COUNT);
  }
  BENCHMARK_ADVANCED(name::serial(name::CREATE_BATCH))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<sc::ecs2::World>(meter.runs(), PAGES);
    meter.measure([&](int run) {
      create_all(*worlds[static_cast<size_t>(run)], ENTITY_COUNT, Position{0.0f, 1.0f}, Velocity{1.0f, 2.0f});
    });
  };

  {
    // One system, whose threads create their entities in pages of their own
    // and give them components right away.
    Scheduler spawner;
    spawner.add([workers](Change<Position, Velocity> change) {
      sc::threading::parallelFor(workers, 1, [&](size_t first, size_t last) {
        for (size_t worker = first; worker < last; ++worker) {
          const Slice part = slice(ENTITY_COUNT, worker, workers);
          for (size_t i = part.first; i < part.last; ++i) {
            const Entity entity = change.create();
            change.emplace<Position>(entity, static_cast<float>(i), 1.0f);
            change.emplace<Velocity>(entity, 1.0f, 2.0f);
          }
        }
      });
    });
    {
      sc::ecs2::World world{PAGES};
      spawner.run(world);
      size_t count = 0;
      world.each<const Position, const Velocity>([&](const Position &, const Velocity &) { ++count; });
      REQUIRE(count == ENTITY_COUNT);
    }
    BENCHMARK_ADVANCED(name::parallel(name::CREATE))(Catch::Benchmark::Chronometer meter) {
      auto worlds = make_worlds<sc::ecs2::World>(meter.runs(), PAGES);
      meter.measure([&](int run) { spawner.run(*worlds[static_cast<size_t>(run)]); });
    };
  }

  BENCHMARK_ADVANCED(name::serial(name::DESTROY))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<World>(meter.runs(), ENTITY_COUNT);
    for (auto &world : worlds) {
      world->fill_moving();
    }
    meter.measure([&](int run) {
      World &world = *worlds[static_cast<size_t>(run)];
      for (const Entity entity : world.entities) {
        world.world.destroy(entity);
      }
    });
  };

  {
    Scheduler reaper;
    reaper.addParallel([](Entity entity, Commands commands, const Position &) { commands.destroy(entity); });
    {
      World world{ENTITY_COUNT};
      world.fill_moving();
      reaper.run(world.world);
      REQUIRE(world.world.column<Position>().pages() == 0);
      REQUIRE(world.world.column<Velocity>().pages() == 0);
      REQUIRE_FALSE(world.world.valid(world.entities.front()));
    }
    BENCHMARK_ADVANCED(name::parallel(name::DESTROY))(Catch::Benchmark::Chronometer meter) {
      auto worlds = make_worlds<World>(meter.runs(), ENTITY_COUNT);
      for (auto &world : worlds) {
        world->fill_moving();
      }
      meter.measure([&](int run) { reaper.run(worlds[static_cast<size_t>(run)]->world); });
    };
  }

  {
    World world{ENTITY_COUNT};
    world.fill_moving();
    BENCHMARK(name::serial(name::ADD_REMOVE)) {
      for (const Entity entity : world.entities) {
        world.world.emplace<Health>(entity, 50.0f);
      }
      for (const Entity entity : world.entities) {
        world.world.erase<Health>(entity);
      }
    };

    Scheduler adder;
    adder.addParallel(
        [](Entity entity, Change<Health> change, const Position &) { change.emplace<Health>(entity, 50.0f); });
    Scheduler remover;
    remover.addParallel([](Entity entity, Change<Health> change, const Position &) { change.erase<Health>(entity); });
    adder.run(world.world);
    REQUIRE(world.world.column<Health>().pages() == world.world.column<Position>().pages());
    remover.run(world.world);
    REQUIRE(world.world.column<Health>().pages() == 0);
    BENCHMARK(name::parallel(name::ADD_REMOVE)) {
      adder.run(world.world);
      remover.run(world.world);
    };
  }

  {
    const std::vector<size_t> order = shuffled_indices(ENTITY_COUNT);
    World world{ENTITY_COUNT};
    world.fill();
    BENCHMARK(name::serial(name::GET)) {
      float sum = 0.0f;
      for (const size_t i : order) {
        sum += world.world.get<const Position>(world.entities[i]).x;
      }
      return sum;
    };

    BENCHMARK(name::serial(name::ITERATE_1)) {
      world.world.each<Position>([](Position &pos) { nudge(pos); });
    };
    BENCHMARK(name::serial(name::ITERATE_2)) {
      world.world.each<Position, const Velocity>([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    };
    BENCHMARK(name::serial(name::ITERATE_3)) {
      world.world.each<Score, const Position, const Health>(
          [](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); });
    };

    Scheduler one;
    one.addParallel([](Position &pos) { nudge(pos); });
    BENCHMARK(name::parallel(name::ITERATE_1)) { one.run(world.world); };
    Scheduler two;
    two.addParallel([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    BENCHMARK(name::parallel(name::ITERATE_2)) { two.run(world.world); };
    Scheduler three;
    three.addParallel([](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); });
    BENCHMARK(name::parallel(name::ITERATE_3)) { three.run(world.world); };
  }

  {
    World world{ENTITY_COUNT};
    world.fill_fragmented();
    BENCHMARK(name::serial(name::ITERATE_FRAGMENTED)) {
      world.world.each<Position, const Velocity>([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    };
    Scheduler parallel;
    parallel.addParallel([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    BENCHMARK(name::parallel(name::ITERATE_FRAGMENTED)) { parallel.run(world.world); };
  }

  for (const auto &[every, benchmark] :
       {std::pair{size_t{2}, name::ITERATE_EVERY_2}, std::pair{size_t{10}, name::ITERATE_EVERY_10},
        std::pair{size_t{100}, name::ITERATE_EVERY_100}}) {
    World world{ENTITY_COUNT};
    for (size_t i = 0; i < world.entities.size(); ++i) {
      world.world.emplace<Position>(world.entities[i], static_cast<float>(i), 1.0f);
      if (i % every == 0) {
        world.world.emplace<Health>(world.entities[i], 50.0f);
      }
    }
    BENCHMARK(name::serial(benchmark)) {
      world.world.each<Health, const Position>([](Health &health, const Position &pos) { heal(health, pos); });
    };
    Scheduler parallel;
    parallel.addParallel([](Health &health, const Position &pos) { heal(health, pos); });
    BENCHMARK(name::parallel(benchmark)) { parallel.run(world.world); };
  }
}

TEST_CASE(LARGE_WORLD, "[ecs_compare][!benchmark]") {
  sc::threading::init(thread_count() - 1);

  World world{PARALLEL_ENTITY_COUNT};
  world.fill();
  sc::ecs2::World &ecs = world.world;

  BENCHMARK(name::serial(name::LOOP_LIGHT)) {
    ecs.each<Position, const Velocity>([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
  };
  {
    Scheduler parallel;
    parallel.addParallel([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    BENCHMARK(name::parallel(name::LOOP_LIGHT)) { parallel.run(ecs); };
  }

  BENCHMARK(name::serial(name::LOOP_HEAVY)) {
    ecs.each<Position, const Velocity>([](Position &pos, const Velocity &vel) { simulate(pos, vel); });
  };
  {
    Scheduler parallel;
    parallel.addParallel([](Position &pos, const Velocity &vel) { simulate(pos, vel); });
    BENCHMARK(name::parallel(name::LOOP_HEAVY)) { parallel.run(ecs); };
  }

  BENCHMARK(name::serial(name::FRAME)) {
    ecs.each<Position, const Velocity>([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    ecs.each<Health, const Velocity>([](Health &health, const Velocity &vel) { regenerate(health, vel); });
    ecs.each<Timer, const Velocity>([](Timer &timer, const Velocity &vel) { tick(timer, vel); });
    ecs.each<Energy, const Velocity>([](Energy &energy, const Velocity &vel) { charge(energy, vel); });
    ecs.each<Score, const Position, const Health>(
        [](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); });
  };
  {
    Scheduler parallel;
    parallel.addParallel([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    parallel.addParallel([](Health &health, const Velocity &vel) { regenerate(health, vel); });
    parallel.addParallel([](Timer &timer, const Velocity &vel) { tick(timer, vel); });
    parallel.addParallel([](Energy &energy, const Velocity &vel) { charge(energy, vel); });
    parallel.addParallel([](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); });
    BENCHMARK(name::parallel(name::FRAME)) { parallel.run(ecs); };
  }
  REQUIRE(ecs.get<const Timer>(world.entities.back()).elapsed > 0.0f);

  {
    const std::vector<size_t> order = shuffled_indices(PARALLEL_ENTITY_COUNT);
    world.fill_targets(order);
    using Followers = Query<Velocity, const Position, const Target>;
    const Followers followers{ecs};
    BENCHMARK(name::serial(name::LOOP_LOOKUP)) {
      followers.each([&](Velocity &vel, const Position &pos, const Target &target) {
        steer(vel, pos, followers.get<const Position>(target.entity));
      });
    };
    Scheduler parallel;
    parallel.add([](Followers query) {
      query.parallelEach([&](Velocity &vel, const Position &pos, const Target &target) {
        steer(vel, pos, query.get<const Position>(target.entity));
      });
    });
    BENCHMARK(name::parallel(name::LOOP_LOOKUP)) { parallel.run(ecs); };

    const Entity last = world.entities.back();
    REQUIRE(steered(ecs.get<const Velocity>(last), ecs.get<const Position>(last),
                    ecs.get<const Position>(world.entities[order.back()])));
  }
}

namespace {

  struct Steady {
    using Handle = Entity;
    sc::ecs2::World world{PAGES};

    Handle spawn(size_t number) {
      const Entity entity = world.create();
      world.emplace<Position>(entity, static_cast<float>(number), 1.0f);
      world.emplace<Velocity>(entity, 1.0f, 2.0f);
      if (number % 2 == 0) {
        world.emplace<Health>(entity, 50.0f);
      }
      return entity;
    }
    void destroy(Handle entity) { world.destroy(entity); }
    void add_timer(Handle entity) { world.emplace<Timer>(entity, 0.0f); }
    void remove_timer(Handle entity) { world.erase<Timer>(entity); }
    void frame() {
      world.each<Position, const Velocity>([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
      world.each<Health, const Velocity>([](Health &health, const Velocity &vel) { regenerate(health, vel); });
      world.each<Timer, const Velocity>([](Timer &timer, const Velocity &vel) { tick(timer, vel); });
    }
  };

} // namespace

TEST_CASE(STEADY_WORLD, "[ecs_compare][!benchmark]") {
  sc::threading::init(thread_count() - 1);
  Steady steady;
  report(run_steady(steady));
  size_t count = 0;
  steady.world.each<const Position>([&](const Position &) { ++count; });
  REQUIRE(count == ENTITY_COUNT);
}

namespace {

  struct Heavy {
    using Handle = Entity;
    sc::ecs2::World world{PAGES};

    Handle spawn() {
      const Entity entity = world.create();
      world.emplace<Transform>(entity, Transform{});
      world.emplace<Body>(entity, Body{1.0f, {}});
      return entity;
    }
    void spawn_all(size_t count) { create_all(world, count, Transform{}, Body{1.0f, {}}); }
    void add_health(Handle entity) { world.emplace<Health>(entity, 50.0f); }
    void remove_health(Handle entity) { world.erase<Health>(entity); }
    void spin() {
      world.each<Transform>([](Transform &transform) { compare::spin(transform); });
    }
    void weigh() {
      world.each<Body>([](Body &body) { compare::weigh(body); });
    }
    void drop() {
      world.each<Transform, const Body>([](Transform &transform, const Body &body) { compare::drop(transform, body); });
    }
  };

} // namespace

TEST_CASE(HEAVY_WORLD, "[ecs_compare][!benchmark]") {
  sc::threading::init(thread_count() - 1);
  run_heavy<Heavy>();
}

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
