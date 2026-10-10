#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

#include "entt/entt.hpp"

#include "common.hpp"

using namespace compare;

// A bare entt::registry, with the configuration the project builds EnTT with.
// It has no means to spread a loop over threads, so only the runs on one
// thread are measured.

namespace {

  struct Target {
    entt::entity entity;
  };

  struct World {
    entt::registry registry;
    std::vector<entt::entity> entities;

    explicit World(size_t count) : entities(count) { registry.create(entities.begin(), entities.end()); }

    void fill_moving() {
      for (size_t i = 0; i < entities.size(); ++i) {
        registry.emplace<Position>(entities[i], static_cast<float>(i), 1.0f);
        registry.emplace<Velocity>(entities[i], 1.0f, 2.0f);
      }
    }

    void fill() {
      fill_moving();
      for (const auto entity : entities) {
        registry.emplace<Health>(entity, 50.0f);
        registry.emplace<Timer>(entity, 0.0f);
        registry.emplace<Energy>(entity, 0.0f);
        registry.emplace<Score>(entity, 0.0f);
      }
    }

    // Entity i gets the entity order[i] as its target.
    void fill_targets(const std::vector<size_t> &order) {
      for (size_t i = 0; i < entities.size(); ++i) {
        registry.emplace<Target>(entities[i], entities[order[i]]);
      }
    }

    void fill_fragmented() {
      fill_moving();
      for (size_t i = 0; i < entities.size(); ++i) {
        for_each_extra(combination(i),
                       [&]<typename Component>(Component) { registry.emplace<Component>(entities[i], 0.0f); });
      }
    }
  };

  // The entities in one call and each component in another.
  template <typename First, typename Second>
  void create_all(entt::registry &registry, size_t count, const First &first, const Second &second) {
    std::vector<entt::entity> entities(count);
    registry.create(entities.begin(), entities.end());
    registry.insert<First>(entities.begin(), entities.end(), first);
    registry.insert<Second>(entities.begin(), entities.end(), second);
  }

} // namespace

// The systems of a frame over one group that owns all six components. A
// component can only be owned once, and four of the systems use Velocity, so
// this is the only way for all of them to go through packed storages. It takes
// that every entity of the systems has all six.
namespace {

  constexpr auto GROUP_INTEGRATE = [](Position &pos, const Velocity &vel, Health &, Timer &, Energy &, Score &) {
    integrate(pos, vel);
  };
  constexpr auto GROUP_REGENERATE = [](Position &, const Velocity &vel, Health &health, Timer &, Energy &, Score &) {
    regenerate(health, vel);
  };
  constexpr auto GROUP_TICK = [](Position &, const Velocity &vel, Health &, Timer &timer, Energy &, Score &) {
    tick(timer, vel);
  };
  constexpr auto GROUP_CHARGE = [](Position &, const Velocity &vel, Health &, Timer &, Energy &energy, Score &) {
    charge(energy, vel);
  };
  constexpr auto GROUP_RATE = [](Position &pos, const Velocity &, Health &health, Timer &, Energy &, Score &score) {
    rate(score, pos, health);
  };

} // namespace

TEST_CASE(SMALL_WORLD, "[ecs_compare][!benchmark]") {
  BENCHMARK_ADVANCED(name::serial(name::CREATE))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<entt::registry>(meter.runs());
    meter.measure([&](int run) {
      auto &registry = *worlds[static_cast<size_t>(run)];
      for (size_t i = 0; i < ENTITY_COUNT; ++i) {
        const entt::entity entity = registry.create();
        registry.emplace<Position>(entity, static_cast<float>(i), 1.0f);
        registry.emplace<Velocity>(entity, 1.0f, 2.0f);
      }
    });
  };

  {
    entt::registry registry;
    create_all(registry, ENTITY_COUNT, Position{0.0f, 1.0f}, Velocity{1.0f, 2.0f});
    size_t count = 0;
    registry.view<const Position, const Velocity>().each(
        [&](const Position &pos, const Velocity &vel) { count += pos.y == 1.0f && vel.dy == 2.0f; });
    REQUIRE(count == ENTITY_COUNT);
  }
  BENCHMARK_ADVANCED(name::serial(name::CREATE_BATCH))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<entt::registry>(meter.runs());
    meter.measure([&](int run) {
      create_all(*worlds[static_cast<size_t>(run)], ENTITY_COUNT, Position{0.0f, 1.0f}, Velocity{1.0f, 2.0f});
    });
  };

  BENCHMARK_ADVANCED(name::serial(name::DESTROY))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<World>(meter.runs(), ENTITY_COUNT);
    for (auto &world : worlds) {
      world->fill_moving();
    }
    meter.measure([&](int run) {
      World &world = *worlds[static_cast<size_t>(run)];
      for (const auto entity : world.entities) {
        world.registry.destroy(entity);
      }
    });
  };

  {
    World world{ENTITY_COUNT};
    world.fill_moving();
    BENCHMARK(name::serial(name::ADD_REMOVE)) {
      for (const auto entity : world.entities) {
        world.registry.emplace<Health>(entity, 50.0f);
      }
      for (const auto entity : world.entities) {
        world.registry.erase<Health>(entity);
      }
    };
  }

  {
    const std::vector<size_t> order = shuffled_indices(ENTITY_COUNT);
    World world{ENTITY_COUNT};
    world.fill();
    BENCHMARK(name::serial(name::GET)) {
      float sum = 0.0f;
      for (const size_t i : order) {
        sum += world.registry.get<const Position>(world.entities[i]).x;
      }
      return sum;
    };
    BENCHMARK(name::serial(name::ITERATE_1)) {
      world.registry.view<Position>().each([](Position &pos) { nudge(pos); });
    };
    BENCHMARK(name::serial(name::ITERATE_2)) {
      world.registry.view<Position, const Velocity>().each(
          [](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    };
    BENCHMARK(name::serial(name::ITERATE_3)) {
      world.registry.view<Score, const Position, const Health>().each(
          [](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); });
    };
  }

  {
    World world{ENTITY_COUNT};
    world.fill_fragmented();
    BENCHMARK(name::serial(name::ITERATE_FRAGMENTED)) {
      world.registry.view<Position, const Velocity>().each(
          [](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    };
  }

  {
    World world{ENTITY_COUNT};
    world.fill();
    auto group = world.registry.group<Position, Velocity>();
    BENCHMARK(name::serial(name::ITERATE_GROUP)) {
      group.each([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    };
  }
}

TEST_CASE(LARGE_WORLD, "[ecs_compare][!benchmark]") {
  World world{PARALLEL_ENTITY_COUNT};
  world.fill();

  BENCHMARK(name::serial(name::LOOP_LIGHT)) {
    world.registry.view<Position, const Velocity>().each(
        [](Position &pos, const Velocity &vel) { integrate(pos, vel); });
  };

  BENCHMARK(name::serial(name::LOOP_HEAVY)) {
    world.registry.view<Position, const Velocity>().each(
        [](Position &pos, const Velocity &vel) { simulate(pos, vel); });
  };

  BENCHMARK(name::serial(name::FRAME)) {
    world.registry.view<Position, const Velocity>().each(
        [](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    world.registry.view<Health, const Velocity>().each(
        [](Health &health, const Velocity &vel) { regenerate(health, vel); });
    world.registry.view<Timer, const Velocity>().each([](Timer &timer, const Velocity &vel) { tick(timer, vel); });
    world.registry.view<Energy, const Velocity>().each(
        [](Energy &energy, const Velocity &vel) { charge(energy, vel); });
    world.registry.view<Score, const Position, const Health>().each(
        [](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); });
  };
  REQUIRE(world.registry.get<const Timer>(world.entities.back()).elapsed > 0.0f);

  {
    const std::vector<size_t> order = shuffled_indices(PARALLEL_ENTITY_COUNT);
    world.fill_targets(order);
    BENCHMARK(name::serial(name::LOOP_LOOKUP)) {
      auto view = world.registry.view<Velocity, const Position, const Target>();
      view.each([&](Velocity &vel, const Position &pos, const Target &target) {
        steer(vel, pos, view.get<const Position>(target.entity));
      });
    };

    const entt::entity last = world.entities.back();
    REQUIRE(steered(world.registry.get<const Velocity>(last), world.registry.get<const Position>(last),
                    world.registry.get<const Position>(world.entities[order.back()])));
  }

  // Last: the group reorders the storages the views above went through.
  auto group = world.registry.group<Position, Velocity>();
  BENCHMARK(name::serial(name::LOOP_GROUP)) {
    group.each([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
  };

  // A world of its own, in which one group owns everything.
  {
    World grouped{PARALLEL_ENTITY_COUNT};
    grouped.fill();

    auto all = grouped.registry.group<Position, const Velocity, Health, Timer, Energy, Score>();
    BENCHMARK(name::serial(name::FRAME_GROUP)) {
      all.each(GROUP_INTEGRATE);
      all.each(GROUP_REGENERATE);
      all.each(GROUP_TICK);
      all.each(GROUP_CHARGE);
      all.each(GROUP_RATE);
    };
    REQUIRE(grouped.registry.get<const Timer>(grouped.entities.back()).elapsed > 0.0f);
  }
}

namespace {

  struct Steady {
    using Handle = entt::entity;
    entt::registry registry;

    Handle spawn(size_t number) {
      const entt::entity entity = registry.create();
      registry.emplace<Position>(entity, static_cast<float>(number), 1.0f);
      registry.emplace<Velocity>(entity, 1.0f, 2.0f);
      if (number % 2 == 0) {
        registry.emplace<Health>(entity, 50.0f);
      }
      return entity;
    }
    void destroy(Handle entity) { registry.destroy(entity); }
    void add_timer(Handle entity) { registry.emplace<Timer>(entity, 0.0f); }
    void remove_timer(Handle entity) { registry.erase<Timer>(entity); }
    void frame() {
      registry.view<Position, const Velocity>().each([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
      registry.view<Health, const Velocity>().each(
          [](Health &health, const Velocity &vel) { regenerate(health, vel); });
      registry.view<Timer, const Velocity>().each([](Timer &timer, const Velocity &vel) { tick(timer, vel); });
    }
  };

} // namespace

TEST_CASE(STEADY_WORLD, "[ecs_compare][!benchmark]") {
  Steady steady;
  report(run_steady(steady));
  REQUIRE(steady.registry.view<Position>().size() == ENTITY_COUNT);
}

namespace {

  struct Heavy {
    using Handle = entt::entity;
    entt::registry registry;

    Handle spawn() {
      const entt::entity entity = registry.create();
      registry.emplace<Transform>(entity, Transform{});
      registry.emplace<Body>(entity, Body{1.0f, {}});
      return entity;
    }
    void spawn_all(size_t count) { create_all(registry, count, Transform{}, Body{1.0f, {}}); }
    void add_health(Handle entity) { registry.emplace<Health>(entity, 50.0f); }
    void remove_health(Handle entity) { registry.erase<Health>(entity); }
    void spin() {
      registry.view<Transform>().each([](Transform &transform) { compare::spin(transform); });
    }
    void weigh() {
      registry.view<Body>().each([](Body &body) { compare::weigh(body); });
    }
    void drop() {
      registry.view<Transform, const Body>().each(
          [](Transform &transform, const Body &body) { compare::drop(transform, body); });
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
