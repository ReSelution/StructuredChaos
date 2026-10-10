#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <flecs.h>

#include "common.hpp"

using namespace compare;

namespace {

  struct Target {
    flecs::entity_t entity;
  };

  struct World {
    flecs::world world;
    std::vector<flecs::entity> entities;

    // The components are known before anything is measured.
    World() {
      world.component<Position>();
      world.component<Velocity>();
      world.component<Health>();
      world.component<Timer>();
      world.component<Energy>();
      world.component<Score>();
      world.component<Target>();
    }

    explicit World(size_t count) : World() {
      entities.reserve(count);
      for (size_t i = 0; i < count; ++i) {
        entities.push_back(world.entity());
      }
    }

    void fill_moving() {
      for (size_t i = 0; i < entities.size(); ++i) {
        entities[i].set<Position>({static_cast<float>(i), 1.0f}).set<Velocity>({1.0f, 2.0f});
      }
    }

    void fill() {
      fill_moving();
      for (const auto &entity : entities) {
        entity.set<Health>({50.0f}).set<Timer>({0.0f}).set<Energy>({0.0f}).set<Score>({0.0f});
      }
    }

    // Entity i gets the entity order[i] as its target.
    void fill_targets(const std::vector<size_t> &order) {
      for (size_t i = 0; i < entities.size(); ++i) {
        entities[i].set<Target>({entities[order[i]].id()});
      }
    }

    void fill_fragmented() {
      fill_moving();
      for (size_t i = 0; i < entities.size(); ++i) {
        for_each_extra(combination(i), [&]<typename Component>(Component) { entities[i].set<Component>({0.0f}); });
      }
    }
  };

  // ecs_bulk_init wants the values of all entities in an array. It is built
  // with the first call, which Catch2 makes before it measures.
  template <typename Component> void *repeated(const Component &value, size_t count) {
    static std::vector<Component> values;
    if (values.size() != count) {
      values.assign(count, value);
    }
    return values.data();
  }

  // All entities and their components in one call.
  template <typename First, typename Second>
  void create_all(flecs::world &world, size_t count, const First &first, const Second &second) {
    void *data[] = {repeated(first, count), repeated(second, count)};
    ecs_bulk_desc_t desc = {};
    desc.count = static_cast<int32_t>(count);
    desc.ids[0] = world.id<First>();
    desc.ids[1] = world.id<Second>();
    desc.data = data;
    ecs_bulk_init(world, &desc);
  }

} // namespace

// On all threads, every benchmark is a frame of a world with systems that are
// marked multi_threaded: only the pipeline spreads a system over threads. What
// such a system changes in the structure of the world is deferred by flecs and
// merged when the threads are done.
TEST_CASE(SMALL_WORLD, "[ecs_compare][!benchmark]") {
  const auto threads = static_cast<int32_t>(thread_count());

  BENCHMARK_ADVANCED(name::serial(name::CREATE))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<World>(meter.runs());
    meter.measure([&](int run) {
      flecs::world &world = worlds[static_cast<size_t>(run)]->world;
      for (size_t i = 0; i < ENTITY_COUNT; ++i) {
        world.entity().set<Position>({static_cast<float>(i), 1.0f}).set<Velocity>({1.0f, 2.0f});
      }
    });
  };

  {
    World world;
    create_all(world.world, ENTITY_COUNT, Position{0.0f, 1.0f}, Velocity{1.0f, 2.0f});
    size_t count = 0;
    world.world.query<const Position, const Velocity>().each(
        [&](const Position &pos, const Velocity &vel) { count += pos.y == 1.0f && vel.dy == 2.0f; });
    REQUIRE(count == ENTITY_COUNT);
  }
  BENCHMARK_ADVANCED(name::serial(name::CREATE_BATCH))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<World>(meter.runs());
    meter.measure([&](int run) {
      create_all(worlds[static_cast<size_t>(run)]->world, ENTITY_COUNT, Position{0.0f, 1.0f}, Velocity{1.0f, 2.0f});
    });
  };

  // Not on all threads: flecs 4.1 puts a new entity into a table of the world
  // right away (flecs_add_to_root_table), which it rules out while the systems
  // run on several threads.

  BENCHMARK_ADVANCED(name::serial(name::DESTROY))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<World>(meter.runs(), ENTITY_COUNT);
    for (auto &world : worlds) {
      world->fill_moving();
    }
    meter.measure([&](int run) {
      World &world = *worlds[static_cast<size_t>(run)];
      for (const auto &entity : world.entities) {
        entity.destruct();
      }
    });
  };

  {
    const auto prepare = [threads](World &world) {
      world.fill_moving();
      world.world.set_threads(threads);
      world.world.system<const Position>("reap").multi_threaded().each(
          [](flecs::entity entity, const Position &) { entity.destruct(); });
    };
    {
      World world{ENTITY_COUNT};
      prepare(world);
      world.world.progress(DT);
      REQUIRE(world.world.count<Position>() == 0);
      REQUIRE_FALSE(world.entities.front().is_alive());
    }
    BENCHMARK_ADVANCED(name::parallel(name::DESTROY))(Catch::Benchmark::Chronometer meter) {
      auto worlds = make_worlds<World>(meter.runs(), ENTITY_COUNT);
      for (auto &world : worlds) {
        prepare(*world);
      }
      meter.measure([&](int run) { worlds[static_cast<size_t>(run)]->world.progress(DT); });
    };
  }

  {
    World world{ENTITY_COUNT};
    world.fill_moving();
    BENCHMARK(name::serial(name::ADD_REMOVE)) {
      for (const auto &entity : world.entities) {
        entity.set<Health>({50.0f});
      }
      for (const auto &entity : world.entities) {
        entity.remove<Health>();
      }
    };

    // The first system says that it writes Health, so that what it deferred
    // is merged before the second one looks for the entities that have it.
    flecs::world &ecs = world.world;
    ecs.set_threads(threads);
    const flecs::system add = ecs.system<const Position>("add").write<Health>().multi_threaded().each(
        [](flecs::entity entity, const Position &) { entity.set<Health>({50.0f}); });
    const flecs::system remove = ecs.system<const Health>("remove").multi_threaded().each(
        [](flecs::entity entity, const Health &) { entity.remove<Health>(); });
    remove.disable();
    ecs.progress(DT);
    REQUIRE(ecs.count<Health>() == static_cast<int32_t>(ENTITY_COUNT));
    remove.enable();
    add.disable();
    ecs.progress(DT);
    REQUIRE(ecs.count<Health>() == 0);
    add.enable();
    BENCHMARK(name::parallel(name::ADD_REMOVE)) { ecs.progress(DT); };
    REQUIRE(ecs.count<Health>() == 0);
  }

  {
    const std::vector<size_t> order = shuffled_indices(ENTITY_COUNT);
    World world{ENTITY_COUNT};
    world.fill();
    BENCHMARK(name::serial(name::GET)) {
      float sum = 0.0f;
      for (const size_t i : order) {
        sum += world.entities[i].get<Position>().x;
      }
      return sum;
    };

    // Queries are made once and kept, as flecs means them to be.
    flecs::world &ecs = world.world;
    auto one = ecs.query<Position>();
    BENCHMARK(name::serial(name::ITERATE_1)) {
      one.each([](Position &pos) { nudge(pos); });
    };
    auto two = ecs.query<Position, const Velocity>();
    BENCHMARK(name::serial(name::ITERATE_2)) {
      two.each([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    };
    auto three = ecs.query<Score, const Position, const Health>();
    BENCHMARK(name::serial(name::ITERATE_3)) {
      three.each([](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); });
    };

    ecs.set_threads(threads);
    const std::vector<flecs::system> systems{
        ecs.system<Position>("one").multi_threaded().each([](Position &pos) { nudge(pos); }),
        ecs.system<Position, const Velocity>("two").multi_threaded().each(
            [](Position &pos, const Velocity &vel) { integrate(pos, vel); }),
        ecs.system<Score, const Position, const Health>("three").multi_threaded().each(
            [](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); }),
    };
    const auto only = [&](size_t enabled) {
      for (size_t i = 0; i < systems.size(); ++i) {
        i == enabled ? systems[i].enable() : systems[i].disable();
      }
    };
    only(0);
    BENCHMARK(name::parallel(name::ITERATE_1)) { ecs.progress(DT); };
    only(1);
    BENCHMARK(name::parallel(name::ITERATE_2)) { ecs.progress(DT); };
    only(2);
    BENCHMARK(name::parallel(name::ITERATE_3)) { ecs.progress(DT); };
  }

  {
    World world{ENTITY_COUNT};
    world.fill_fragmented();
    flecs::world &ecs = world.world;
    auto query = ecs.query<Position, const Velocity>();
    BENCHMARK(name::serial(name::ITERATE_FRAGMENTED)) {
      query.each([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    };

    ecs.set_threads(threads);
    ecs.system<Position, const Velocity>("two").multi_threaded().each(
        [](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    BENCHMARK(name::parallel(name::ITERATE_FRAGMENTED)) { ecs.progress(DT); };
  }
}

// Only the pipeline spreads a system over threads, so every benchmark here is
// a frame of a world in which just the systems in question are enabled.
TEST_CASE(LARGE_WORLD, "[ecs_compare][!benchmark]") {
  const auto threads = static_cast<int32_t>(thread_count());

  World world{PARALLEL_ENTITY_COUNT};
  world.fill();
  flecs::world &ecs = world.world;

  const flecs::system light = ecs.system<Position, const Velocity>("light").multi_threaded().each(
      [](Position &pos, const Velocity &vel) { integrate(pos, vel); });
  const flecs::system heavy = ecs.system<Position, const Velocity>("heavy").multi_threaded().each(
      [](Position &pos, const Velocity &vel) { simulate(pos, vel); });
  const std::vector<flecs::system> frame{
      ecs.system<Position, const Velocity>("integrate").multi_threaded().each([](Position &pos, const Velocity &vel) {
        integrate(pos, vel);
      }),
      ecs.system<Health, const Velocity>("regenerate").multi_threaded().each([](Health &health, const Velocity &vel) {
        regenerate(health, vel);
      }),
      ecs.system<Timer, const Velocity>("tick").multi_threaded().each(
          [](Timer &timer, const Velocity &vel) { tick(timer, vel); }),
      ecs.system<Energy, const Velocity>("charge").multi_threaded().each(
          [](Energy &energy, const Velocity &vel) { charge(energy, vel); }),
      ecs.system<Score, const Position, const Health>("rate").multi_threaded().each(
          [](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); }),
  };

  // The entity is of the stage of the thread that runs the system, and so is
  // what is looked up through its world.
  const flecs::system lookup = ecs.system<Velocity, const Position, const Target>("lookup").multi_threaded().each(
      [](flecs::entity entity, Velocity &vel, const Position &pos, const Target &target) {
        steer(vel, pos, entity.world().entity(target.entity).get<Position>());
      });

  const auto enable = [&](bool with_light, bool with_heavy, bool with_frame, bool with_lookup = false) {
    with_light ? light.enable() : light.disable();
    with_heavy ? heavy.enable() : heavy.disable();
    with_lookup ? lookup.enable() : lookup.disable();
    for (const flecs::system &system : frame) {
      with_frame ? system.enable() : system.disable();
    }
  };

  enable(true, false, false);
  ecs.set_threads(0);
  BENCHMARK(name::serial(name::LOOP_LIGHT)) { ecs.progress(DT); };
  ecs.set_threads(threads);
  BENCHMARK(name::parallel(name::LOOP_LIGHT)) { ecs.progress(DT); };

  enable(false, true, false);
  ecs.set_threads(0);
  BENCHMARK(name::serial(name::LOOP_HEAVY)) { ecs.progress(DT); };
  ecs.set_threads(threads);
  BENCHMARK(name::parallel(name::LOOP_HEAVY)) { ecs.progress(DT); };

  enable(false, false, true);
  ecs.set_threads(0);
  BENCHMARK(name::serial(name::FRAME)) { ecs.progress(DT); };
  ecs.set_threads(threads);
  BENCHMARK(name::parallel(name::FRAME)) { ecs.progress(DT); };
  REQUIRE(world.entities.back().get<Timer>().elapsed > 0.0f);

  // Only now, so that the benchmarks above run on the world they always had.
  const std::vector<size_t> order = shuffled_indices(PARALLEL_ENTITY_COUNT);
  world.fill_targets(order);
  enable(false, false, false, true);
  ecs.set_threads(0);
  BENCHMARK(name::serial(name::LOOP_LOOKUP)) { ecs.progress(DT); };
  ecs.set_threads(threads);
  BENCHMARK(name::parallel(name::LOOP_LOOKUP)) { ecs.progress(DT); };
  const flecs::entity last = world.entities.back();
  REQUIRE(steered(last.get<Velocity>(), last.get<Position>(), world.entities[order.back()].get<Position>()));
}

namespace {

  struct Steady {
    using Handle = flecs::entity;
    World base;
    flecs::world &world = base.world;
    flecs::query<Position, const Velocity> moving = world.query<Position, const Velocity>();
    flecs::query<Health, const Velocity> healing = world.query<Health, const Velocity>();
    flecs::query<Timer, const Velocity> timed = world.query<Timer, const Velocity>();

    Handle spawn(size_t number) {
      const flecs::entity entity =
          world.entity().set<Position>({static_cast<float>(number), 1.0f}).set<Velocity>({1.0f, 2.0f});
      if (number % 2 == 0) {
        entity.set<Health>({50.0f});
      }
      return entity;
    }
    void destroy(Handle entity) { entity.destruct(); }
    void add_timer(Handle entity) { entity.set<Timer>({0.0f}); }
    void remove_timer(Handle entity) { entity.remove<Timer>(); }
    void frame() {
      moving.each([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
      healing.each([](Health &health, const Velocity &vel) { regenerate(health, vel); });
      timed.each([](Timer &timer, const Velocity &vel) { tick(timer, vel); });
    }
  };

} // namespace

TEST_CASE(STEADY_WORLD, "[ecs_compare][!benchmark]") {
  Steady steady;
  report(run_steady(steady));
  REQUIRE(static_cast<size_t>(steady.world.count<Position>()) == ENTITY_COUNT);
}

namespace {

  struct Heavy {
    using Handle = flecs::entity;
    flecs::world world;
    flecs::query<Transform> transforms = world.query<Transform>();
    flecs::query<Body> bodies = world.query<Body>();
    flecs::query<Transform, const Body> both = world.query<Transform, const Body>();

    // The components are known before anything is measured.
    Heavy() { world.component<Health>(); }

    Handle spawn() { return world.entity().set<Transform>(Transform{}).set<Body>(Body{1.0f, {}}); }
    void spawn_all(size_t count) { create_all(world, count, Transform{}, Body{1.0f, {}}); }
    void add_health(Handle entity) { entity.set<Health>({50.0f}); }
    void remove_health(Handle entity) { entity.remove<Health>(); }
    void spin() {
      transforms.each([](Transform &transform) { compare::spin(transform); });
    }
    void weigh() {
      bodies.each([](Body &body) { compare::weigh(body); });
    }
    void drop() {
      both.each([](Transform &transform, const Body &body) { compare::drop(transform, body); });
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
