#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// gaia includes <windows.h>, whose min and max would take the place of
// std::min and std::max in common.hpp.
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <gaia.h>

#include "common.hpp"

using namespace compare;
namespace ecs = gaia::ecs;

namespace {

  struct Target {
    ecs::Entity entity;
  };

  struct World {
    ecs::World world;
    std::vector<ecs::Entity> entities;

    // The components are known before anything is measured.
    World() {
      (void) world.add<Position>();
      (void) world.add<Velocity>();
      (void) world.add<Health>();
      (void) world.add<Timer>();
      (void) world.add<Energy>();
      (void) world.add<Score>();
      (void) world.add<Target>();
    }

    explicit World(size_t count) : World() {
      entities.reserve(count);
      for (size_t i = 0; i < count; ++i) {
        entities.push_back(world.add());
      }
    }

    void fill_moving() {
      for (size_t i = 0; i < entities.size(); ++i) {
        world.add<Position>(entities[i], {static_cast<float>(i), 1.0f});
        world.add<Velocity>(entities[i], {1.0f, 2.0f});
      }
    }

    void fill() {
      fill_moving();
      for (const ecs::Entity entity: entities) {
        world.add<Health>(entity, {50.0f});
        world.add<Timer>(entity, {0.0f});
        world.add<Energy>(entity, {0.0f});
        world.add<Score>(entity, {0.0f});
      }
    }

    // Entity i gets the entity order[i] as its target.
    void fill_targets(const std::vector<size_t> &order) {
      for (size_t i = 0; i < entities.size(); ++i) {
        world.add<Target>(entities[i], {entities[order[i]]});
      }
    }

    void fill_fragmented() {
      fill_moving();
      for (size_t i = 0; i < entities.size(); ++i) {
        for_each_extra(combination(i),
                       [&]<typename Component>(Component) { world.add<Component>(entities[i], {0.0f}); });
      }
    }
  };

  // The five systems of a frame, for World::update to run.
  void add_frame_systems(ecs::World &world, ecs::QueryExecType mode) {
    world.system().all<Position &>().all<Velocity>().mode(mode).on_each(
        [](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    world.system().all<Health &>().all<Velocity>().mode(mode).on_each(
        [](Health &health, const Velocity &vel) { regenerate(health, vel); });
    world.system().all<Timer &>().all<Velocity>().mode(mode).on_each(
        [](Timer &timer, const Velocity &vel) { tick(timer, vel); });
    world.system().all<Energy &>().all<Velocity>().mode(mode).on_each(
        [](Energy &energy, const Velocity &vel) { charge(energy, vel); });
    world.system().all<Score &>().all<Position>().all<Health>().mode(mode).on_each(
        [](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); });
  }

  // One entity with the components and all others as copies of it, in one
  // call.
  template<typename First, typename Second>
  void create_all(ecs::World &world, size_t count, const First &first, const Second &second) {
    const ecs::Entity entity = world.add();
    world.add<First>(entity, first);
    world.add<Second>(entity, second);
    world.copy_n(entity, static_cast<uint32_t>(count - 1));
  }

} // namespace

// On all threads, a loop is a query run with QueryExecType::Parallel. What it
// changes in the structure of the world goes through the command buffer of
// the iterator that is safe to use from several threads, which gaia-ecs
// commits once the query is done.
TEST_CASE(SMALL_WORLD, "[ecs_compare][!benchmark]") {
  // The calling thread is one of them.
  const uint32_t threads = thread_count();
  gaia::mt::ThreadPool &pool = gaia::mt::ThreadPool::get();
  pool.set_max_workers(threads, threads);

  BENCHMARK_ADVANCED(name::serial(name::CREATE))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<World>(meter.runs());
    meter.measure([&](int run) {
      ecs::World &world = worlds[static_cast<size_t>(run)]->world;
      for (size_t i = 0; i < ENTITY_COUNT; ++i) {
        const ecs::Entity entity = world.add();
        world.add<Position>(entity, {static_cast<float>(i), 1.0f});
        world.add<Velocity>(entity, {1.0f, 2.0f});
      }
    });
  };

  {
    World world;
    create_all(world.world, ENTITY_COUNT, Position{0.0f, 1.0f}, Velocity{1.0f, 2.0f});
    size_t count = 0;
    world.world.query().all<Position>().all<Velocity>().each(
        [&](const Position &pos, const Velocity &vel) { count += pos.y == 1.0f && vel.dy == 2.0f; });
    REQUIRE(count == ENTITY_COUNT);
  }
  BENCHMARK_ADVANCED(name::serial(name::CREATE_BATCH))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<World>(meter.runs());
    meter.measure([&](int run) {
      create_all(worlds[static_cast<size_t>(run)]->world, ENTITY_COUNT, Position{0.0f, 1.0f}, Velocity{1.0f, 2.0f});
    });
  };

  {
    // There are no entities to loop over yet, so the thread pool spreads the
    // numbers of the new ones over the threads.
    const auto spawn = [&pool, threads](ecs::World &world) {
      ecs::CommandBufferMT commands(world);
      gaia::mt::JobParallel job{[&commands](const gaia::mt::JobArgs &args) {
        for (uint32_t i = args.idxStart; i < args.idxEnd; ++i) {
          const ecs::Entity entity = commands.add();
          commands.add<Position>(entity, {static_cast<float>(i), 1.0f});
          commands.add<Velocity>(entity, {1.0f, 2.0f});
        }
      }};
      pool.wait(pool.sched_par(std::move(job), static_cast<uint32_t>(ENTITY_COUNT),
                               static_cast<uint32_t>(ENTITY_COUNT) / threads));
      commands.commit();
    };
    {
      World world;
      spawn(world.world);
      REQUIRE(world.world.query().all<Position>().all<Velocity>().count() == ENTITY_COUNT);
    }
    BENCHMARK_ADVANCED(name::parallel(name::CREATE))(Catch::Benchmark::Chronometer meter) {
      auto worlds = make_worlds<World>(meter.runs());
      meter.measure([&](int run) { spawn(worlds[static_cast<size_t>(run)]->world); });
    };
  }

  BENCHMARK_ADVANCED(name::serial(name::DESTROY))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<World>(meter.runs(), ENTITY_COUNT);
    for (auto &world: worlds) {
      world->fill_moving();
    }
    meter.measure([&](int run) {
      World &world = *worlds[static_cast<size_t>(run)];
      for (const ecs::Entity entity: world.entities) {
        world.world.del(entity);
      }
    });
  };

  {
    const auto reap = [](ecs::World &world) {
      world.query().all<Position>().each(
          [](ecs::Iter &it) {
            ecs::CommandBufferMT &commands = it.cmd_buffer_mt();
            const auto entities = it.view<ecs::Entity>();
            GAIA_EACH(it) commands.del(entities[i]);
          },
          ecs::QueryExecType::Parallel);
    };
    {
      World world{ENTITY_COUNT};
      world.fill_moving();
      reap(world.world);
      REQUIRE(world.world.query().all<Position>().count() == 0);
      REQUIRE_FALSE(world.world.valid(world.entities.front()));
    }
    BENCHMARK_ADVANCED(name::parallel(name::DESTROY))(Catch::Benchmark::Chronometer meter) {
      auto worlds = make_worlds<World>(meter.runs(), ENTITY_COUNT);
      for (auto &world: worlds) {
        world->fill_moving();
      }
      meter.measure([&](int run) { reap(worlds[static_cast<size_t>(run)]->world); });
    };
  }

  {
    World world{ENTITY_COUNT};
    world.fill_moving();
    BENCHMARK(name::serial(name::ADD_REMOVE)) {
      for (const ecs::Entity entity: world.entities) {
        world.world.add<Health>(entity, {50.0f});
      }
      for (const ecs::Entity entity: world.entities) {
        world.world.del<Health>(entity);
      }
    };

    ecs::World &w = world.world;
    ecs::Query all = w.query().all<Position>();
    ecs::Query healthy = w.query().all<Health>();
    const auto add = [&] {
      all.each(
          [](ecs::Iter &it) {
            ecs::CommandBufferMT &commands = it.cmd_buffer_mt();
            const auto entities = it.view<ecs::Entity>();
            GAIA_EACH(it) commands.add<Health>(entities[i], Health{50.0f});
          },
          ecs::QueryExecType::Parallel);
    };
    const auto remove = [&] {
      healthy.each(
          [](ecs::Iter &it) {
            ecs::CommandBufferMT &commands = it.cmd_buffer_mt();
            const auto entities = it.view<ecs::Entity>();
            GAIA_EACH(it) commands.del<Health>(entities[i]);
          },
          ecs::QueryExecType::Parallel);
    };
    add();
    REQUIRE(healthy.count() == ENTITY_COUNT);
    remove();
    REQUIRE(healthy.count() == 0);
    BENCHMARK(name::parallel(name::ADD_REMOVE)) {
      add();
      remove();
    };
    REQUIRE(healthy.count() == 0);
  }

  {
    const std::vector<size_t> order = shuffled_indices(ENTITY_COUNT);
    World world{ENTITY_COUNT};
    world.fill();
    BENCHMARK(name::serial(name::GET)) {
      float sum = 0.0f;
      for (const size_t i: order) {
        sum += world.world.get<Position>(world.entities[i]).x;
      }
      return sum;
    };

    // Queries are made once and kept, as gaia-ecs means them to be.
    ecs::Query one = world.world.query().all<Position &>();
    ecs::Query two = world.world.query().all<Position &>().all<Velocity>();
    ecs::Query three = world.world.query().all<Score &>().all<Position>().all<Health>();
    for (const auto mode: {ecs::QueryExecType::Default, ecs::QueryExecType::Parallel}) {
      const auto named = [mode](const char *benchmark) {
        return mode == ecs::QueryExecType::Default ? name::serial(benchmark) : name::parallel(benchmark);
      };
      BENCHMARK(named(name::ITERATE_1)) {
        one.each([](Position &pos) { nudge(pos); }, mode);
      };
      BENCHMARK(named(name::ITERATE_2)) {
        two.each([](Position &pos, const Velocity &vel) { integrate(pos, vel); }, mode);
      };
      BENCHMARK(named(name::ITERATE_3)) {
        three.each([](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); }, mode);
      };
    }
  }

  {
    World world{ENTITY_COUNT};
    world.fill_fragmented();
    ecs::Query query = world.world.query().all<Position &>().all<Velocity>();
    BENCHMARK(name::serial(name::ITERATE_FRAGMENTED)) {
      query.each([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    };
    BENCHMARK(name::parallel(name::ITERATE_FRAGMENTED)) {
      query.each([](Position &pos, const Velocity &vel) { integrate(pos, vel); }, ecs::QueryExecType::Parallel);
    };
  }
}

TEST_CASE(LARGE_WORLD, "[ecs_compare][!benchmark]") {
  // The calling thread is one of them.
  const uint32_t threads = thread_count();
  gaia::mt::ThreadPool::get().set_max_workers(threads, threads);

  {
    World world{PARALLEL_ENTITY_COUNT};
    world.fill();
    ecs::Query query = world.world.query().all<Position &>().all<Velocity>();

    BENCHMARK(name::serial(name::LOOP_LIGHT)) {
      query.each([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    };
    BENCHMARK(name::parallel(name::LOOP_LIGHT)) {
      query.each([](Position &pos, const Velocity &vel) { integrate(pos, vel); }, ecs::QueryExecType::Parallel);
    };

    BENCHMARK(name::serial(name::LOOP_HEAVY)) {
      query.each([](Position &pos, const Velocity &vel) { simulate(pos, vel); });
    };
    BENCHMARK(name::parallel(name::LOOP_HEAVY)) {
      query.each([](Position &pos, const Velocity &vel) { simulate(pos, vel); }, ecs::QueryExecType::Parallel);
    };
  }

  {
    World world{PARALLEL_ENTITY_COUNT};
    world.fill();
    add_frame_systems(world.world, ecs::QueryExecType::Default);
    BENCHMARK(name::serial(name::FRAME)) { world.world.update(); };
    REQUIRE(world.world.get<Timer>(world.entities.back()).elapsed > 0.0f);
  }

  {
    World world{PARALLEL_ENTITY_COUNT};
    world.fill();
    add_frame_systems(world.world, ecs::QueryExecType::Parallel);
    BENCHMARK(name::parallel(name::FRAME)) { world.world.update(); };
    REQUIRE(world.world.get<Timer>(world.entities.back()).elapsed > 0.0f);
  }

  {
    const std::vector<size_t> order = shuffled_indices(PARALLEL_ENTITY_COUNT);
    World world{PARALLEL_ENTITY_COUNT};
    world.fill();
    world.fill_targets(order);
    ecs::World &w = world.world;
    ecs::Query query = w.query().all<Velocity &>().all<Position>().all<Target>();
    const auto follow = [&w](Velocity &vel, const Position &pos, const Target &target) {
      steer(vel, pos, w.get<Position>(target.entity));
    };

    BENCHMARK(name::serial(name::LOOP_LOOKUP)) { query.each(follow); };
    BENCHMARK(name::parallel(name::LOOP_LOOKUP)) { query.each(follow, ecs::QueryExecType::Parallel); };

    const ecs::Entity last = world.entities.back();
    REQUIRE(steered(w.get<Velocity>(last), w.get<Position>(last), w.get<Position>(world.entities[order.back()])));
  }
}

namespace {

  struct Steady {
    using Handle = ecs::Entity;
    World base;
    ecs::World &world = base.world;
    ecs::Query moving = world.query().all<Position &>().all<Velocity>();
    ecs::Query healing = world.query().all<Health &>().all<Velocity>();
    ecs::Query timed = world.query().all<Timer &>().all<Velocity>();

    Handle spawn(size_t number) {
      const ecs::Entity entity = world.add();
      world.add<Position>(entity, {static_cast<float>(number), 1.0f});
      world.add<Velocity>(entity, {1.0f, 2.0f});
      if (number % 2 == 0) {
        world.add<Health>(entity, {50.0f});
      }
      return entity;
    }
    void destroy(Handle entity) { world.del(entity); }
    void add_timer(Handle entity) { world.add<Timer>(entity, {0.0f}); }
    void remove_timer(Handle entity) { world.del<Timer>(entity); }
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
  REQUIRE(steady.world.query().all<Position>().count() == ENTITY_COUNT);
}

namespace {

  struct Heavy {
    using Handle = ecs::Entity;
    ecs::World world;
    ecs::Query transforms = world.query().all<Transform &>();
    ecs::Query bodies = world.query().all<Body &>();
    ecs::Query both = world.query().all<Transform &>().all<Body>();

    // The components are known before anything is measured.
    Heavy() { (void) world.add<Health>(); }

    Handle spawn() {
      const ecs::Entity entity = world.add();
      world.add<Transform>(entity, Transform{});
      world.add<Body>(entity, Body{1.0f, {}});
      return entity;
    }
    void spawn_all(size_t count) { create_all(world, count, Transform{}, Body{1.0f, {}}); }
    void add_health(Handle entity) { world.add<Health>(entity, {50.0f}); }
    void remove_health(Handle entity) { world.del<Health>(entity); }
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
