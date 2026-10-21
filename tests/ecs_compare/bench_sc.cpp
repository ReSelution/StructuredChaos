#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "sc/ecs/scheduler.hpp"
#include "sc/ecs/world.hpp"
#include "sc/threading/threading.hpp"

#include "common.hpp"

using namespace compare;

// On all threads, a loop is a system added with addParallel. Components are
// added and removed by the loop itself, which the scheduler treats as writing
// them; entities are destroyed through Commands, once the loop is over.

namespace {

  using sc::ecs::Commands;
  using sc::ecs::Edit;
  using sc::ecs::Entity;
  using sc::ecs::Query;
  using sc::ecs::Scheduler;
  using sc::ecs::Spawn;

  // The default, which each column chooses its pages with.
  constexpr sc::ecs::Pages PAGES = sc::ecs::Pages::Auto;

  struct Target {
    Entity entity;
  };

  struct World {
    sc::ecs::World world{PAGES};
    std::vector<Entity> entities;

    explicit World(size_t count) : entities(count) {
      for (Entity &entity: entities) {
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
      for (const Entity entity: entities) {
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
  template<typename First, typename Second>
  void create_all(sc::ecs::World &world, size_t count, const First &first, const Second &second) {
    std::vector<Entity> entities(count);
    world.create(entities, first, second);
  }

} // namespace

TEST_CASE(SMALL_WORLD, "[ecs_compare][!benchmark]") {
  // The thread that starts a loop works on it as well.
  const size_t workers = thread_count();
  sc::threading::init(static_cast<uint32_t>(workers) - 1);

  BENCHMARK_ADVANCED(name::serial(name::CREATE))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<sc::ecs::World>(meter.runs(), PAGES);
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
    sc::ecs::World world{PAGES};
    create_all(world, ENTITY_COUNT, Position{0.0f, 1.0f}, Velocity{1.0f, 2.0f});
    size_t count = 0;
    world.each<const Position, const Velocity>(
        [&](const Position &pos, const Velocity &vel) { count += pos.y == 1.0f && vel.dy == 2.0f; });
    REQUIRE(count == ENTITY_COUNT);
  }
  BENCHMARK_ADVANCED(name::serial(name::CREATE_BATCH))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<sc::ecs::World>(meter.runs(), PAGES);
    meter.measure([&](int run) {
      create_all(*worlds[static_cast<size_t>(run)], ENTITY_COUNT, Position{0.0f, 1.0f}, Velocity{1.0f, 2.0f});
    });
  };

  // Filled and emptied the way the benchmark fills them, so that the indices
  // come back from the same free list.
  BENCHMARK_ADVANCED(name::serial(name::CREATE_AGAIN))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<sc::ecs::World>(meter.runs(), PAGES);
    std::vector<Entity> entities(ENTITY_COUNT);
    for (auto &world: worlds) {
      for (Entity &entity: entities) {
        entity = world->create();
        world->emplace<Position>(entity, 0.0f, 1.0f);
        world->emplace<Velocity>(entity, 1.0f, 2.0f);
      }
      world->destroy(std::span<const Entity>{entities});
    }
    meter.measure([&](int run) {
      auto &world = *worlds[static_cast<size_t>(run)];
      for (size_t i = 0; i < ENTITY_COUNT; ++i) {
        const Entity entity = world.create();
        world.emplace<Position>(entity, static_cast<float>(i), 1.0f);
        world.emplace<Velocity>(entity, 1.0f, 2.0f);
      }
    });
  };
  BENCHMARK_ADVANCED(name::serial(name::CREATE_BATCH_AGAIN))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<sc::ecs::World>(meter.runs(), PAGES);
    std::vector<Entity> entities(ENTITY_COUNT);
    for (auto &world: worlds) {
      world->create(entities, Position{0.0f, 1.0f}, Velocity{1.0f, 2.0f});
      world->destroy(std::span<const Entity>{entities});
    }
    meter.measure([&](int run) {
      create_all(*worlds[static_cast<size_t>(run)], ENTITY_COUNT, Position{0.0f, 1.0f}, Velocity{1.0f, 2.0f});
    });
  };

  {
    // One system, whose threads create their entities in pages of their own
    // and give them components right away.
    Scheduler spawner;
    spawner.add([workers](Edit<Position, Velocity> edit, Spawn spawn) {
      sc::threading::parallelFor(workers, 1, [&](size_t first, size_t last) {
        for (size_t worker = first; worker < last; ++worker) {
          const Slice part = slice(ENTITY_COUNT, worker, workers);
          for (size_t i = part.first; i < part.last; ++i) {
            const Entity entity = spawn.create();
            edit.emplace<Position>(entity, static_cast<float>(i), 1.0f);
            edit.emplace<Velocity>(entity, 1.0f, 2.0f);
          }
        }
      });
    });
    {
      sc::ecs::World world{PAGES};
      spawner.run(world);
      size_t count = 0;
      world.each<const Position, const Velocity>([&](const Position &, const Velocity &) { ++count; });
      REQUIRE(count == ENTITY_COUNT);
    }
    BENCHMARK_ADVANCED(name::parallel(name::CREATE))(Catch::Benchmark::Chronometer meter) {
      auto worlds = make_worlds<sc::ecs::World>(meter.runs(), PAGES);
      meter.measure([&](int run) { spawner.run(*worlds[static_cast<size_t>(run)]); });
    };
  }

  BENCHMARK_ADVANCED(name::serial(name::DESTROY))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<World>(meter.runs(), ENTITY_COUNT);
    for (auto &world: worlds) {
      world->fill_moving();
    }
    meter.measure([&](int run) {
      World &world = *worlds[static_cast<size_t>(run)];
      for (const Entity entity: world.entities) {
        world.world.destroy(entity);
      }
    });
  };

  {
    World world{ENTITY_COUNT};
    world.fill_moving();
    REQUIRE(world.world.destroy(std::span<const Entity>{world.entities}) == ENTITY_COUNT);
    REQUIRE(world.world.column<Position>().pages() == world.world.column<Position>().spares());
    REQUIRE_FALSE(world.world.valid(world.entities.back()));
  }
  BENCHMARK_ADVANCED(name::serial(name::DESTROY_BATCH))(Catch::Benchmark::Chronometer meter) {
    auto worlds = make_worlds<World>(meter.runs(), ENTITY_COUNT);
    for (auto &world: worlds) {
      world->fill_moving();
    }
    meter.measure([&](int run) {
      World &world = *worlds[static_cast<size_t>(run)];
      world.world.destroy(std::span<const Entity>{world.entities});
    });
  };

  {
    Scheduler reaper;
    reaper.addParallel([](Entity entity, Commands commands, const Position &) { commands.destroy(entity); });
    {
      World world{ENTITY_COUNT};
      world.fill_moving();
      reaper.run(world.world);
      REQUIRE(world.world.column<Position>().pages() == world.world.column<Position>().spares());
      REQUIRE(world.world.column<Velocity>().pages() == world.world.column<Velocity>().spares());
      REQUIRE_FALSE(world.world.valid(world.entities.front()));
    }
    BENCHMARK_ADVANCED(name::parallel(name::DESTROY))(Catch::Benchmark::Chronometer meter) {
      auto worlds = make_worlds<World>(meter.runs(), ENTITY_COUNT);
      for (auto &world: worlds) {
        world->fill_moving();
      }
      meter.measure([&](int run) { reaper.run(worlds[static_cast<size_t>(run)]->world); });
    };
  }

  {
    World world{ENTITY_COUNT};
    world.fill_moving();
    BENCHMARK(name::serial(name::ADD_REMOVE)) {
      for (const Entity entity: world.entities) {
        world.world.emplace<Health>(entity, 50.0f);
      }
      for (const Entity entity: world.entities) {
        world.world.erase<Health>(entity);
      }
    };

    Scheduler adder;
    adder.addParallel([](Entity entity, Edit<Health> edit, const Position &) { edit.emplace<Health>(entity, 50.0f); });
    Scheduler remover;
    remover.addParallel([](Entity entity, Edit<Health> edit, const Position &) { edit.erase<Health>(entity); });
    adder.run(world.world);
    REQUIRE(world.world.column<Health>().pages() == world.world.column<Position>().pages());
    remover.run(world.world);
    REQUIRE(world.world.column<Health>().pages() == world.world.column<Health>().spares());
    BENCHMARK(name::parallel(name::ADD_REMOVE)) {
      adder.run(world.world);
      remover.run(world.world);
    };

    Scheduler recordAdd;
    recordAdd.addParallel(
        [](Entity entity, Commands commands, const Position &) { commands.emplace<Health>(entity, 50.0f); });
    Scheduler recordRemove;
    recordRemove.addParallel(
        [](Entity entity, Commands commands, const Position &) { commands.erase<Health>(entity); });
    recordAdd.run(world.world);
    REQUIRE(world.world.column<Health>().pages() == world.world.column<Position>().pages());
    REQUIRE(world.world.get<const Health>(world.entities.back()).value == 50.0f);
    recordRemove.run(world.world);
    REQUIRE(world.world.column<Health>().pages() == world.world.column<Health>().spares());
    REQUIRE_FALSE(world.world.contains<Health>(world.entities.back()));
    BENCHMARK(name::parallel(name::ADD_REMOVE_RECORDED)) {
      recordAdd.run(world.world);
      recordRemove.run(world.world);
    };
  }

  {
    const std::vector<size_t> order = shuffled_indices(ENTITY_COUNT);
    World world{ENTITY_COUNT};
    world.fill();
    BENCHMARK(name::serial(name::GET)) {
      float sum = 0.0f;
      for (const size_t i: order) {
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

  for (const auto &[every, benchmark]:
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
  sc::ecs::World &ecs = world.world;

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

  {
    constexpr size_t FEW = 1000;
    constexpr size_t HITS_EACH = PARALLEL_ENTITY_COUNT / FEW;
    static_assert(HITS_EACH * FEW == PARALLEL_ENTITY_COUNT);
    const Entity boss = world.entities.front();
    const auto damage = [](Health &health) { health.value -= 1.0f; };

    Scheduler random;
    random.addParallel([&](Edit<Health> edit, const Target &target) { edit.modify(target.entity, damage); });
    Scheduler few;
    few.addParallel([&](Entity entity, Edit<Health> edit, const Target &) {
      edit.modify(world.entities[sc::ecs::indexOf(entity) % FEW], damage);
    });
    Scheduler one;
    one.addParallel([&](Edit<Health> edit, const Target &) { edit.modify(boss, damage); });
    std::atomic<uint32_t> counter = 0;
    Scheduler atomic;
    atomic.addParallel([&](const Target &) { counter.fetch_add(1, std::memory_order_relaxed); });

    // Every hit arrives, on all threads. From 0, where the floats are exact.
    for (size_t i = 0; i < FEW; ++i) {
      ecs.get<Health>(world.entities[i]).value = 0.0f;
    }
    few.run(ecs);
    for (size_t i = 0; i < FEW; ++i) {
      REQUIRE(ecs.get<const Health>(world.entities[i]).value == -static_cast<float>(HITS_EACH));
    }
    ecs.get<Health>(boss).value = 0.0f;
    one.run(ecs);
    REQUIRE(ecs.get<const Health>(boss).value == -static_cast<float>(PARALLEL_ENTITY_COUNT));

    BENCHMARK(name::serial(name::WRITE_RANDOM)) {
      ecs.each<const Target>([&](const Target &target) { damage(ecs.get<Health>(target.entity)); });
    };
    BENCHMARK(name::serial(name::MODIFY_RANDOM)) {
      ecs.each<const Target>([&](const Target &target) { ecs.modify<Health>(target.entity, damage); });
    };
    BENCHMARK(name::parallel(name::MODIFY_RANDOM)) { random.run(ecs); };
    BENCHMARK(name::parallel(name::MODIFY_FEW)) { few.run(ecs); };
    BENCHMARK(name::serial(name::MODIFY_ONE)) {
      ecs.each<const Target>([&](const Target &) { ecs.modify<Health>(boss, damage); });
    };
    BENCHMARK(name::parallel(name::MODIFY_ONE)) { one.run(ecs); };
    BENCHMARK(name::parallel(name::ATOMIC_ONE)) { atomic.run(ecs); };

    // Recorded, and done one after the other once the system is over.
    Scheduler recordRandomSerial;
    recordRandomSerial.add([&](Query<const Target> query, Commands commands) {
      query.each([&](const Target &target) { commands.modify(target.entity, damage); });
    });
    Scheduler recordRandom;
    recordRandom.addParallel([&](Commands commands, const Target &target) { commands.modify(target.entity, damage); });
    Scheduler recordFew;
    recordFew.addParallel([&](Entity entity, Commands commands, const Target &) {
      commands.modify(world.entities[sc::ecs::indexOf(entity) % FEW], damage);
    });
    Scheduler recordOne;
    recordOne.addParallel([&](Commands commands, const Target &) { commands.modify(boss, damage); });

    for (size_t i = 0; i < FEW; ++i) {
      ecs.get<Health>(world.entities[i]).value = 0.0f;
    }
    recordFew.run(ecs);
    for (size_t i = 0; i < FEW; ++i) {
      REQUIRE(ecs.get<const Health>(world.entities[i]).value == -static_cast<float>(HITS_EACH));
    }
    ecs.get<Health>(boss).value = 0.0f;
    recordOne.run(ecs);
    REQUIRE(ecs.get<const Health>(boss).value == -static_cast<float>(PARALLEL_ENTITY_COUNT));

    BENCHMARK(name::serial(name::RECORD_RANDOM)) { recordRandomSerial.run(ecs); };
    BENCHMARK(name::parallel(name::RECORD_RANDOM)) { recordRandom.run(ecs); };
    BENCHMARK(name::parallel(name::RECORD_FEW)) { recordFew.run(ecs); };
    BENCHMARK(name::parallel(name::RECORD_ONE)) { recordOne.run(ecs); };
  }
}

namespace {

  struct Steady {
    using Handle = Entity;
    sc::ecs::World world{PAGES};

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
    sc::ecs::World world{PAGES};

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

namespace {

  // Entity i gets set i % 3, see SETS_AFTER_WORLD: with create and emplace,
  // or created with its components, which puts every set in an area of its
  // own.
  std::unique_ptr<sc::ecs::World> make_sets(bool with) {
    auto world = std::make_unique<sc::ecs::World>(PAGES);
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      const Position pos{static_cast<float>(i), 1.0f};
      if (with) {
        switch (i % 3) {
          case 0:
            (void) world->create(pos, Velocity{1.0f, 2.0f});
            break;
          case 1:
            (void) world->create(pos, Health{50.0f});
            break;
          default:
            (void) world->create(pos);
        }
        continue;
      }
      const Entity entity = world->create();
      world->emplace<Position>(entity, pos);
      switch (i % 3) {
        case 0:
          world->emplace<Velocity>(entity, 1.0f, 2.0f);
          break;
        case 1:
          world->emplace<Health>(entity, 50.0f);
          break;
        default:
          break;
      }
    }
    return world;
  }

  void run_sets(bool with) {
    const auto made = make_sets(with);
    sc::ecs::World &world = *made;
    size_t first = 0;
    size_t second = 0;
    size_t all = 0;
    world.each<const Position, const Velocity>([&](const Position &, const Velocity &) { ++first; });
    world.each<const Health>([&](const Health &) { ++second; });
    world.each<const Position>([&](const Position &) { ++all; });
    REQUIRE(first == (ENTITY_COUNT + 2) / 3);
    REQUIRE(second == (ENTITY_COUNT + 1) / 3);
    REQUIRE(all == ENTITY_COUNT);

    BENCHMARK(name::serial(name::ITERATE_SET_1)) {
      world.each<Position, const Velocity>([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    };
    BENCHMARK(name::serial(name::ITERATE_SET_2)) {
      world.each<Health>([](Health &health) { heal(health); });
    };
    BENCHMARK(name::serial(name::ITERATE_SETS_ALL)) {
      world.each<Position>([](Position &pos) { nudge(pos); });
    };
    Scheduler set1;
    set1.addParallel([](Position &pos, const Velocity &vel) { integrate(pos, vel); });
    BENCHMARK(name::parallel(name::ITERATE_SET_1)) { set1.run(world); };
    Scheduler set2;
    set2.addParallel([](Health &health) { heal(health); });
    BENCHMARK(name::parallel(name::ITERATE_SET_2)) { set2.run(world); };
    Scheduler sets;
    sets.addParallel([](Position &pos) { nudge(pos); });
    BENCHMARK(name::parallel(name::ITERATE_SETS_ALL)) { sets.run(world); };
  }

} // namespace

TEST_CASE(SETS_AFTER_WORLD, "[ecs_compare][!benchmark]") {
  sc::threading::init(thread_count() - 1);
  run_sets(false);
}

TEST_CASE(SETS_WITH_WORLD, "[ecs_compare][!benchmark]") {
  sc::threading::init(thread_count() - 1);
  run_sets(true);
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
TEST_CASE(memory::SETS_AFTER, "[ecs_compare][.memory]") {
  report_memory(memory::SETS_AFTER, [] { return make_sets(false); });
}
TEST_CASE(memory::SETS_WITH, "[ecs_compare][.memory]") {
  report_memory(memory::SETS_WITH, [] { return make_sets(true); });
}
