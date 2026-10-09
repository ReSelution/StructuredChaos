#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

#include <ginseng/ginseng.hpp>

#include "common.hpp"

using namespace compare;

// ginseng has no means to spread a loop over threads, so only the runs on one
// thread are measured.

namespace {

  using Entity = ginseng::database::ent_id;

  struct Target {
    Entity entity;
  };

  struct World {
    ginseng::database database;
    std::vector<Entity> entities;

    World() = default;

    explicit World(size_t count) {
      entities.reserve(count);
      for (size_t i = 0; i < count; ++i) {
        entities.push_back(database.create_entity());
      }
    }

    void fill_moving() {
      for (size_t i = 0; i < entities.size(); ++i) {
        database.add_component(entities[i], Position{static_cast<float>(i), 1.0f});
        database.add_component(entities[i], Velocity{1.0f, 2.0f});
      }
    }

    void fill() {
      fill_moving();
      for (const Entity entity : entities) {
        database.add_component(entity, Health{50.0f});
        database.add_component(entity, Timer{0.0f});
        database.add_component(entity, Energy{0.0f});
        database.add_component(entity, Score{0.0f});
      }
    }

    // Entity i gets the entity order[i] as its target.
    void fill_targets(const std::vector<size_t> &order) {
      for (size_t i = 0; i < entities.size(); ++i) {
        database.add_component(entities[i], Target{entities[order[i]]});
      }
    }

    void fill_fragmented() {
      fill_moving();
      for (size_t i = 0; i < entities.size(); ++i) {
        for_each_extra(combination(i),
                       [&]<typename Component>(Component) { database.add_component(entities[i], Component{0.0f}); });
      }
    }
  };

} // namespace

TEST_CASE(SMALL_WORLD, "[ecs_compare][!benchmark]") {
  BENCHMARK_ADVANCED(name::serial(name::CREATE))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<World>(meter.runs());
    meter.measure([&](int run) {
      ginseng::database &database = worlds[static_cast<size_t>(run)]->database;
      for (size_t i = 0; i < ENTITY_COUNT; ++i) {
        const Entity entity = database.create_entity();
        database.add_component(entity, Position{static_cast<float>(i), 1.0f});
        database.add_component(entity, Velocity{1.0f, 2.0f});
      }
    });
  };

  BENCHMARK_ADVANCED(name::serial(name::DESTROY))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<World>(meter.runs(), ENTITY_COUNT);
    for (auto &world : worlds) {
      world->fill_moving();
    }
    meter.measure([&](int run) {
      World &world = *worlds[static_cast<size_t>(run)];
      for (const Entity entity : world.entities) {
        world.database.destroy_entity(entity);
      }
    });
  };

  {
    World world{ENTITY_COUNT};
    world.fill_moving();
    BENCHMARK(name::serial(name::ADD_REMOVE)) {
      for (const Entity entity : world.entities) {
        world.database.add_component(entity, Health{50.0f});
      }
      for (const Entity entity : world.entities) {
        world.database.remove_component<Health>(entity);
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
        sum += world.database.get_component<Position>(world.entities[i]).x;
      }
      return sum;
    };
    BENCHMARK(name::serial(name::ITERATE_1)) {
      world.database.visit([](Position &pos) { nudge(pos); });
    };
    BENCHMARK(name::serial(name::ITERATE_2)) {
      world.database.visit([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    };
    BENCHMARK(name::serial(name::ITERATE_3)) {
      world.database.visit([](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); });
    };
  }

  {
    World world{ENTITY_COUNT};
    world.fill_fragmented();
    BENCHMARK(name::serial(name::ITERATE_FRAGMENTED)) {
      world.database.visit([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    };
  }
}

TEST_CASE(LARGE_WORLD, "[ecs_compare][!benchmark]") {
  World world{PARALLEL_ENTITY_COUNT};
  world.fill();

  BENCHMARK(name::serial(name::LOOP_LIGHT)) {
    world.database.visit([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
  };

  BENCHMARK(name::serial(name::LOOP_HEAVY)) {
    world.database.visit([](Position &pos, const Velocity &vel) { simulate(pos, vel); });
  };

  BENCHMARK(name::serial(name::FRAME)) {
    world.database.visit([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    world.database.visit([](Health &health, const Velocity &vel) { regenerate(health, vel); });
    world.database.visit([](Timer &timer, const Velocity &vel) { tick(timer, vel); });
    world.database.visit([](Energy &energy, const Velocity &vel) { charge(energy, vel); });
    world.database.visit([](Score &score, const Position &pos, const Health &health) { rate(score, pos, health); });
  };
  REQUIRE(world.database.get_component<Timer>(world.entities.back()).elapsed > 0.0f);

  const std::vector<size_t> order = shuffled_indices(PARALLEL_ENTITY_COUNT);
  world.fill_targets(order);
  ginseng::database &database = world.database;
  BENCHMARK(name::serial(name::LOOP_LOOKUP)) {
    database.visit([&](Velocity &vel, const Position &pos, const Target &target) {
      steer(vel, pos, database.get_component<Position>(target.entity));
    });
  };
  const Entity last = world.entities.back();
  REQUIRE(steered(database.get_component<Velocity>(last), database.get_component<Position>(last),
                  database.get_component<Position>(world.entities[order.back()])));
}
