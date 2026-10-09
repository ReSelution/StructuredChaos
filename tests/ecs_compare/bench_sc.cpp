#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <utility>
#include <vector>

#include "common.hpp"
#include "sc/ecs/commands.hpp"
#include "sc/ecs/entity.hpp"
#include "sc/ecs/group.hpp"
#include "sc/ecs/registry.hpp"
#include "sc/ecs/scheduler.hpp"
#include "sc/ecs/view.hpp"
#include "sc/threading/threading.hpp"

using namespace compare;

namespace {

  struct Target {
    entt::entity entity;
  };

  struct World {
    sc::ecs::Registry registry;
    std::vector<sc::ecs::Entity> entities;

    explicit World(size_t count) : entities(count) { registry.create(entities.begin(), entities.end()); }

    void fill_moving() {
      for (size_t i = 0; i < entities.size(); ++i) {
        registry.emplace<Position>(entities[i], static_cast<float>(i), 1.0f);
        registry.emplace<Velocity>(entities[i], 1.0f, 2.0f);
      }
    }

    void fill() {
      fill_moving();
      for (const auto &entity : entities) {
        registry.emplace<Health>(entity, 50.0f);
        registry.emplace<Timer>(entity, 0.0f);
        registry.emplace<Energy>(entity, 0.0f);
        registry.emplace<Score>(entity, 0.0f);
      }
    }

    // Entity i gets the entity order[i] as its target.
    void fill_targets(const std::vector<size_t> &order) {
      for (size_t i = 0; i < entities.size(); ++i) {
        registry.emplace<Target>(entities[i], entities[order[i]].entity);
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
  // The thread that runs the scheduler works on the loops as well.
  const size_t workers = thread_count();
  sc::threading::init(static_cast<uint32_t>(workers) - 1);

  // On all threads, a loop is a system added with addParallel, which also
  // records the structural changes to entities that exist. Entities are
  // created by one system per thread, each with Commands of its own, which the
  // scheduler runs at the same time.
  using Moving = sc::ecs::Group<entt::owned_t<Position, const Velocity>>;

  BENCHMARK_ADVANCED(name::serial(name::CREATE))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<sc::ecs::Registry>(meter.runs());
    meter.measure([&](int run) {
      auto &registry = *worlds[static_cast<size_t>(run)];
      for (size_t i = 0; i < ENTITY_COUNT; ++i) {
        const sc::ecs::Entity entity = registry.create();
        registry.emplace<Position>(entity, static_cast<float>(i), 1.0f);
        registry.emplace<Velocity>(entity, 1.0f, 2.0f);
      }
    });
  };

  {
    sc::ecs::Scheduler spawners;
    for (size_t worker = 0; worker < workers; ++worker) {
      spawners.add([part = slice(ENTITY_COUNT, worker, workers)](sc::ecs::Commands<Position, Velocity> commands) {
        for (size_t i = part.first; i < part.last; ++i) {
          const sc::ecs::Entity entity = commands.create();
          commands.emplace<Position>(entity, static_cast<float>(i), 1.0f);
          commands.emplace<Velocity>(entity, 1.0f, 2.0f);
        }
      });
    }
    REQUIRE(spawners.stages().size() == 1);
    {
      sc::ecs::Registry registry;
      spawners.run(registry);
      REQUIRE(registry.view<Position, Velocity>().size_hint() == ENTITY_COUNT);
    }
    BENCHMARK_ADVANCED(name::parallel(name::CREATE))(Catch::Benchmark::Chronometer meter) {
      auto worlds = make_worlds<sc::ecs::Registry>(meter.runs());
      meter.measure([&](int run) { spawners.run(*worlds[static_cast<size_t>(run)]); });
    };
  }

  // Without Commands. The registry lets any thread create entities and one
  // thread at a time write a component type, so every component gets a thread
  // of its own. Destroying an entity writes all components and adding Health
  // only one, so there is nothing to spread for the benchmarks below.
  {
    const auto spawn = [](sc::ecs::Registry &registry, std::vector<sc::ecs::Entity> &entities) {
      registry.create(entities.begin(), entities.end());
      sc::threading::parallelFor(2, 1, [&](size_t first, size_t last) {
        for (size_t component = first; component < last; ++component) {
          for (size_t i = 0; i < entities.size(); ++i) {
            if (component == 0) {
              registry.emplace<Position>(entities[i], static_cast<float>(i), 1.0f);
            } else {
              registry.emplace<Velocity>(entities[i], 1.0f, 2.0f);
            }
          }
        }
      });
    };
    std::vector<sc::ecs::Entity> entities(ENTITY_COUNT);
    {
      sc::ecs::Registry registry;
      spawn(registry, entities);
      REQUIRE(registry.view<Position>().size() == ENTITY_COUNT);
      REQUIRE(registry.view<Velocity>().size() == ENTITY_COUNT);
      REQUIRE(registry.get<const Position>(entities.back()).x == static_cast<float>(ENTITY_COUNT - 1));
    }
    BENCHMARK_ADVANCED(name::parallel(name::CREATE_DIRECT))(Catch::Benchmark::Chronometer meter) {
      auto worlds = make_worlds<sc::ecs::Registry>(meter.runs());
      meter.measure([&](int run) { spawn(*worlds[static_cast<size_t>(run)], entities); });
    };
  }

  BENCHMARK_ADVANCED(name::serial(name::DESTROY))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<World>(meter.runs(), ENTITY_COUNT);
    for (auto &world : worlds) {
      world->fill_moving();
    }
    meter.measure([&](int run) {
      World &world = *worlds[static_cast<size_t>(run)];
      for (const auto &entity : world.entities) {
        world.registry.destroy(entity);
      }
    });
  };

  {
    sc::ecs::Scheduler reapers;
    reapers.addParallel(
        [](entt::entity entity, sc::ecs::Commands<> commands, const Position &) { commands.destroy(entity); });
    {
      World world{ENTITY_COUNT};
      world.fill_moving();
      reapers.run(world.registry);
      REQUIRE(world.registry.view<Position>().size() == 0);
      REQUIRE_FALSE(world.registry.valid(world.entities.front()));
    }
    BENCHMARK_ADVANCED(name::parallel(name::DESTROY))(Catch::Benchmark::Chronometer meter) {
      auto worlds = make_worlds<World>(meter.runs(), ENTITY_COUNT);
      for (auto &world : worlds) {
        world->fill_moving();
      }
      meter.measure([&](int run) { reapers.run(worlds[static_cast<size_t>(run)]->registry); });
    };
  }

  {
    World world{ENTITY_COUNT};
    world.fill_moving();
    BENCHMARK(name::serial(name::ADD_REMOVE)) {
      for (const auto &entity : world.entities) {
        world.registry.emplace<Health>(entity, 50.0f);
      }
      for (const auto &entity : world.entities) {
        world.registry.erase<Health>(entity);
      }
    };

    sc::ecs::Scheduler adders;
    sc::ecs::Scheduler removers;
    adders.addParallel([](entt::entity entity, sc::ecs::Commands<Health> commands, const Position &) {
      commands.emplace<Health>(entity, 50.0f);
    });
    removers.addParallel([](entt::entity entity, sc::ecs::Commands<Health> commands, const Position &) {
      commands.remove<Health>(entity);
    });
    adders.run(world.registry);
    REQUIRE(world.registry.view<Health>().size() == ENTITY_COUNT);
    removers.run(world.registry);
    REQUIRE(world.registry.view<Health>().size() == 0);
    BENCHMARK(name::parallel(name::ADD_REMOVE)) {
      adders.run(world.registry);
      removers.run(world.registry);
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
      sc::ecs::each(world.registry.view<Position>(), [](Position &pos) { nudge(pos); });
    };
    BENCHMARK(name::serial(name::ITERATE_2)) {
      sc::ecs::each(world.registry.view<Position, const Velocity>(),
                    [](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    };
    BENCHMARK(name::serial(name::ITERATE_3)) {
      sc::ecs::each(world.registry.view<Score, const Position, const Health>(),
                    [](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); });
    };

    sc::ecs::Scheduler one;
    one.addParallel([](Position &pos) { nudge(pos); });
    BENCHMARK(name::parallel(name::ITERATE_1)) { one.run(world.registry); };
    sc::ecs::Scheduler two;
    two.addParallel([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    BENCHMARK(name::parallel(name::ITERATE_2)) { two.run(world.registry); };
    sc::ecs::Scheduler three;
    three.addParallel([](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); });
    BENCHMARK(name::parallel(name::ITERATE_3)) { three.run(world.registry); };
  }

  {
    World world{ENTITY_COUNT};
    world.fill_fragmented();
    BENCHMARK(name::serial(name::ITERATE_FRAGMENTED)) {
      sc::ecs::each(world.registry.view<Position, const Velocity>(),
                    [](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    };
    sc::ecs::Scheduler parallel;
    parallel.addParallel([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    BENCHMARK(name::parallel(name::ITERATE_FRAGMENTED)) { parallel.run(world.registry); };
  }

  {
    World world{ENTITY_COUNT};
    world.fill();
    auto group = world.registry.group<Position, Velocity>();
    BENCHMARK(name::serial(name::ITERATE_GROUP)) {
      sc::ecs::each(group, [](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    };
    sc::ecs::Scheduler parallel;
    parallel.addParallel<Moving>([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    BENCHMARK(name::parallel(name::ITERATE_GROUP)) { parallel.run(world.registry); };
  }

  for (const auto &[every, benchmark] :
       {std::pair{size_t{2}, name::ITERATE_EVERY_2}, std::pair{size_t{10}, name::ITERATE_EVERY_10},
        std::pair{size_t{100}, name::ITERATE_EVERY_100}}) {
    World world{ENTITY_COUNT};
    for (size_t i = 0; i < world.entities.size(); ++i) {
      world.registry.emplace<Position>(world.entities[i], static_cast<float>(i), 1.0f);
      if (i % every == 0) {
        world.registry.emplace<Health>(world.entities[i], 50.0f);
      }
    }
    BENCHMARK(name::serial(benchmark)) {
      sc::ecs::each(world.registry.view<Health, const Position>(),
                    [](Health &health, const Position &pos) { heal(health, pos); });
    };
    sc::ecs::Scheduler parallel;
    parallel.addParallel([](Health &health, const Position &pos) { heal(health, pos); });
    BENCHMARK(name::parallel(benchmark)) { parallel.run(world.registry); };
  }
}

TEST_CASE(LARGE_WORLD, "[ecs_compare][!benchmark]") {
  // The thread that runs the scheduler works on the loops as well.
  sc::threading::init(thread_count() - 1);

  World world{PARALLEL_ENTITY_COUNT};
  world.fill();

  {
    sc::ecs::Scheduler serial;
    serial.add([](sc::ecs::View<Position, const Velocity> view) {
      sc::ecs::each(view, [](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    });
    BENCHMARK(name::serial(name::LOOP_LIGHT)) { serial.run(world.registry); };

    sc::ecs::Scheduler parallel;
    parallel.addParallel([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    BENCHMARK(name::parallel(name::LOOP_LIGHT)) { parallel.run(world.registry); };
  }

  {
    sc::ecs::Scheduler serial;
    serial.add([](sc::ecs::View<Position, const Velocity> view) {
      sc::ecs::each(view, [](Position &pos, const Velocity &vel) { simulate(pos, vel); });
    });
    BENCHMARK(name::serial(name::LOOP_HEAVY)) { serial.run(world.registry); };

    sc::ecs::Scheduler parallel;
    parallel.addParallel([](Position &pos, const Velocity &vel) { simulate(pos, vel); });
    BENCHMARK(name::parallel(name::LOOP_HEAVY)) { parallel.run(world.registry); };
  }

  {
    // A scheduler would run the systems that do not conflict at the same time,
    // so the one thread calls them itself.
    BENCHMARK(name::serial(name::FRAME)) {
      sc::ecs::each(world.registry.view<Position, const Velocity>(),
                    [](Position &pos, const Velocity &vel) { integrate(pos, vel); });
      sc::ecs::each(world.registry.view<Health, const Velocity>(),
                    [](Health &health, const Velocity &vel) { regenerate(health, vel); });
      sc::ecs::each(world.registry.view<Timer, const Velocity>(),
                    [](Timer &timer, const Velocity &vel) { tick(timer, vel); });
      sc::ecs::each(world.registry.view<Energy, const Velocity>(),
                    [](Energy &energy, const Velocity &vel) { charge(energy, vel); });
      sc::ecs::each(world.registry.view<Score, const Position, const Health>(),
                    [](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); });
    };

    sc::ecs::Scheduler parallel;
    parallel.addParallel([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    parallel.addParallel([](Health &health, const Velocity &vel) { regenerate(health, vel); });
    parallel.addParallel([](Timer &timer, const Velocity &vel) { tick(timer, vel); });
    parallel.addParallel([](Energy &energy, const Velocity &vel) { charge(energy, vel); });
    parallel.addParallel([](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); });
    BENCHMARK(name::parallel(name::FRAME)) { parallel.run(world.registry); };
  }
  REQUIRE(world.registry.get<const Timer>(world.entities.back()).elapsed > 0.0f);

  {
    const std::vector<size_t> order = shuffled_indices(PARALLEL_ENTITY_COUNT);
    world.fill_targets(order);
    using Followers = sc::ecs::View<Velocity, const Position, const Target>;

    sc::ecs::Scheduler serial;
    serial.add([](Followers view) {
      sc::ecs::each(view, [&](Velocity &vel, const Position &pos, const Target &target) {
        steer(vel, pos, view.get<const Position>(target.entity));
      });
    });
    BENCHMARK(name::serial(name::LOOP_LOOKUP)) { serial.run(world.registry); };

    sc::ecs::Scheduler parallel;
    parallel.add([](Followers view) {
      sc::ecs::parallelEach(view, [&](Velocity &vel, const Position &pos, const Target &target) {
        steer(vel, pos, view.get<const Position>(target.entity));
      });
    });
    BENCHMARK(name::parallel(name::LOOP_LOOKUP)) { parallel.run(world.registry); };

    const auto &last = world.entities.back();
    REQUIRE(steered(world.registry.get<const Velocity>(last), world.registry.get<const Position>(last),
                    world.registry.get<const Position>(world.entities[order.back()])));
  }

  // Last: the group reorders the storages the views above went through.
  {
    using Moving = sc::ecs::Group<entt::owned_t<Position, const Velocity>>;

    sc::ecs::Scheduler serial;
    serial.add(
        [](Moving group) { sc::ecs::each(group, [](Position &pos, const Velocity &vel) { integrate(pos, vel); }); });
    BENCHMARK(name::serial(name::LOOP_GROUP)) { serial.run(world.registry); };

    sc::ecs::Scheduler parallel;
    parallel.addParallel<Moving>([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    BENCHMARK(name::parallel(name::LOOP_GROUP)) { parallel.run(world.registry); };
  }

  // A world of its own, in which one group owns everything.
  {
    using All = sc::ecs::Group<entt::owned_t<Position, const Velocity, Health, Timer, Energy, Score>>;

    World grouped{PARALLEL_ENTITY_COUNT};
    grouped.fill();

    auto group = grouped.registry.group<Position, const Velocity, Health, Timer, Energy, Score>();
    BENCHMARK(name::serial(name::FRAME_GROUP)) {
      sc::ecs::each(group, GROUP_INTEGRATE);
      sc::ecs::each(group, GROUP_REGENERATE);
      sc::ecs::each(group, GROUP_TICK);
      sc::ecs::each(group, GROUP_CHARGE);
      sc::ecs::each(group, GROUP_RATE);
    };

    // The components of a group are one family to the scheduler, so the
    // systems run one after the other, each of them on all threads.
    sc::ecs::Scheduler parallel;
    parallel.addParallel<All>(GROUP_INTEGRATE);
    parallel.addParallel<All>(GROUP_REGENERATE);
    parallel.addParallel<All>(GROUP_TICK);
    parallel.addParallel<All>(GROUP_CHARGE);
    parallel.addParallel<All>(GROUP_RATE);
    BENCHMARK(name::parallel(name::FRAME_GROUP)) { parallel.run(grouped.registry); };
    REQUIRE(grouped.registry.get<const Timer>(grouped.entities.back()).elapsed > 0.0f);
  }
}
