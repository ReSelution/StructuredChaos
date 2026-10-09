#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// What every executable of the comparison shares: the components, the work of
// the systems and the names of the benchmarks. One executable per library does
// the same work with it, scripts/ecs_compare.py runs them all and puts the
// results side by side.
//
// A benchmark a library has no means for is left out and shows up as a gap.

namespace compare {

  constexpr size_t ENTITY_COUNT = 100'000;
  // The loops that are spread over threads go through more entities, so that
  // there is something to spread.
  constexpr size_t PARALLEL_ENTITY_COUNT = 1'000'000;
  constexpr float DT = 1.0f / 60.0f;

  // Plain aggregates, so that no library has to run a constructor it could
  // otherwise skip.
  struct Position {
    float x;
    float y;
  };

  struct Velocity {
    float dx;
    float dy;
  };

  struct Health {
    float value;
  };

  struct Timer {
    float elapsed;
  };

  struct Energy {
    float value;
  };

  struct Score {
    float value;
  };

  // The work of the systems, the same for every library.
  inline void nudge(Position &pos) { pos.x += DT; }
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
  // Enough arithmetic per entity that the loop is not limited by memory alone.
  inline void simulate(Position &pos, const Velocity &vel) {
    float x = pos.x;
    float y = pos.y;
    for (int step = 0; step < 4; ++step) {
      const float length = std::sqrt(x * x + y * y + 1.0f);
      x = x / length + vel.dx;
      y = y / length + vel.dy;
    }
    pos.x = x;
    pos.y = y;
  }

  inline void heal(Health &health, const Position &pos) { health.value += pos.x * DT; }

  // Needs the position of another entity, which is somewhere else in memory.
  inline void steer(Velocity &vel, const Position &pos, const Position &target) {
    vel.dx = (target.x - pos.x) * DT;
    vel.dy = (target.y - pos.y) * DT;
  }
  // Whether steer was the last to write the velocity, to check that a library
  // looked up the right entity.
  inline bool steered(const Velocity &vel, const Position &pos, const Position &target) {
    Velocity expected{};
    steer(expected, pos, target);
    return vel.dx == expected.dx && vel.dy == expected.dy;
  }

  // One of several components that entities have in varying combinations.
  template <size_t Index> struct Extra {
    float value;
  };
  constexpr size_t EXTRA_COUNT = 8;

  // Which Extra components the entity with the given number has, one bit for
  // each: one of 2^EXTRA_COUNT combinations, spread evenly and without a
  // pattern.
  constexpr uint32_t combination(size_t i) {
    return static_cast<uint32_t>((i * 2654435761u) >> 16) & ((1u << EXTRA_COUNT) - 1u);
  }

  // Calls the function with every Extra component of the combination.
  template <typename Function> void for_each_extra(uint32_t combination, Function &&function) {
    [&]<size_t... Index>(std::index_sequence<Index...>) {
      ((((combination >> Index) & 1u) != 0 ? function(Extra<Index>{}) : void()), ...);
    }(std::make_index_sequence<EXTRA_COUNT>{});
  }

  // The order in which single entities are looked up.
  inline std::vector<size_t> shuffled_indices(size_t count) {
    std::vector<size_t> indices(count);
    for (size_t i = 0; i < count; ++i) {
      indices[i] = i;
    }
    std::shuffle(indices.begin(), indices.end(), std::mt19937{42});
    return indices;
  }

  // One fresh world per run of a sample, built outside the timing.
  template <typename World, typename... Args>
  std::vector<std::unique_ptr<World>> make_worlds(int count, Args &&...args) {
    std::vector<std::unique_ptr<World>> worlds;
    worlds.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
      worlds.push_back(std::make_unique<World>(args...));
    }
    return worlds;
  }

  // The threads a parallel loop may use, the calling one included. Every
  // library gets the same number.
  inline unsigned thread_count() {
    if (const char *value = std::getenv("ECS_COMPARE_THREADS")) {
      const int threads = std::atoi(value);
      if (threads > 0) {
        return static_cast<unsigned>(threads);
      }
    }
    return std::max(1u, std::thread::hardware_concurrency());
  }

  // The part of count items that the given one of several workers takes.
  struct Slice {
    size_t first;
    size_t last;
  };
  constexpr Slice slice(size_t count, size_t worker, size_t workers) {
    return {.first = count * worker / workers, .last = count * (worker + 1) / workers};
  }

  // Where a library spreads work over the entities of a loop, one entity with
  // this component per thread stands for the entities that thread creates.
  struct Spawner {
    uint32_t first;
    uint32_t last;
  };

  // Every benchmark runs on one thread and, where the library has a means for
  // it, on all of them. scripts/ecs_compare.py tells the two apart by the end
  // of the name.
  namespace name {
    inline std::string serial(const char *benchmark) { return std::string{benchmark} + " [1 thread]"; }
    inline std::string parallel(const char *benchmark) { return std::string{benchmark} + " [all threads]"; }

    // ENTITY_COUNT entities. On all threads, structural changes are recorded
    // by the threads and made by the library afterwards, in the way each
    // library provides for it.
    inline constexpr const char *CREATE = "create entities with 2 components";
    // Only sc::ecs: the registry itself instead of recorded changes, which
    // lets one thread at a time write a component type.
    inline constexpr const char *CREATE_DIRECT = "create entities with 2 components, 1 thread per component";
    inline constexpr const char *DESTROY = "destroy entities";
    inline constexpr const char *ADD_REMOVE = "add + remove a component";
    inline constexpr const char *GET = "get 1 component, random order";
    inline constexpr const char *ITERATE_1 = "iterate 1 component";
    inline constexpr const char *ITERATE_2 = "iterate 2 components";
    inline constexpr const char *ITERATE_3 = "iterate 3 components";
    inline constexpr const char *ITERATE_FRAGMENTED = "iterate 2 components, 256 combinations";
    // Every entity has a position, only one in so many has health, spread
    // evenly.
    inline constexpr const char *ITERATE_EVERY_2 = "iterate 2 components, 1 on every 2nd entity";
    inline constexpr const char *ITERATE_EVERY_10 = "iterate 2 components, 1 on every 10th entity";
    inline constexpr const char *ITERATE_EVERY_100 = "iterate 2 components, 1 on every 100th entity";
    // Only for the libraries built on sparse sets, which can keep the storages
    // of some components in the same order.
    inline constexpr const char *ITERATE_GROUP = "iterate 2 components, owning group";

    // PARALLEL_ENTITY_COUNT entities.
    inline constexpr const char *LOOP_LIGHT = "1 system, light work";
    inline constexpr const char *LOOP_HEAVY = "1 system, heavy work";
    inline constexpr const char *LOOP_GROUP = "1 system, light work, owning group";
    inline constexpr const char *FRAME = "5 systems";
    inline constexpr const char *FRAME_GROUP = "5 systems, owning group";
    // Every entity has a Target component, defined by each executable with the
    // entity type of its library, that names another entity in no order.
    inline constexpr const char *LOOP_LOOKUP = "1 system, looks up another entity";
  } // namespace name

  // Names of the test cases, the sections of the report. They state
  // ENTITY_COUNT and PARALLEL_ENTITY_COUNT.
  inline constexpr const char *SMALL_WORLD = "100 000 entities";
  inline constexpr const char *LARGE_WORLD = "1 000 000 entities";
  // ENTITY_COUNT and STEADY_CHANGES.
  inline constexpr const char *STEADY_WORLD = "100 000 entities in use, 1 000 replaced and 1 000 changed per frame";

  // A world that has been in use for a while, unlike the fresh ones of the
  // benchmarks above: every frame some entities are destroyed and as many
  // created, some get a component or lose it, and then the systems run. All
  // of it on one thread. What counts is not only how long a frame takes but
  // also how long the slow ones take, so the frames are timed one by one.
  constexpr size_t STEADY_CHANGES = ENTITY_COUNT / 100;
  constexpr size_t STEADY_FRAMES = 2000;
  // Frames before the measured ones, until the world is no longer fresh.
  constexpr size_t STEADY_WARM_UP = 200;

  // Times of a frame in nanoseconds.
  struct SteadyTimes {
    double median;
    double percentile90;
    double percentile99;
    double slowest;
  };

  // Runs the frames on a world of a library, which is a type with
  //   Handle                     an entity
  //   Handle spawn(size_t n)     an entity with Position and Velocity and, for an even n, Health
  //   void destroy(Handle)
  //   void add_timer(Handle)     adds Timer, which the entity does not have
  //   void remove_timer(Handle)
  //   void frame()               integrate, regenerate and tick for every entity that has the components
  // The changes are the same for every library, in number and order.
  template <typename World> SteadyTimes run_steady(World &world) {
    struct Slot {
      typename World::Handle entity;
      bool timed;
    };
    std::vector<Slot> alive;
    alive.reserve(ENTITY_COUNT);
    size_t number = 0;
    for (; number < ENTITY_COUNT; ++number) {
      alive.push_back({world.spawn(number), false});
    }

    std::mt19937 random{42};
    std::vector<double> times;
    times.reserve(STEADY_FRAMES);
    for (size_t frame = 0; frame < STEADY_WARM_UP + STEADY_FRAMES; ++frame) {
      const auto start = std::chrono::steady_clock::now();
      for (size_t change = 0; change < STEADY_CHANGES; ++change) {
        Slot &slot = alive[random() % alive.size()];
        world.destroy(slot.entity);
        slot = {world.spawn(number++), false};
      }
      for (size_t change = 0; change < STEADY_CHANGES; ++change) {
        Slot &slot = alive[random() % alive.size()];
        slot.timed ? world.remove_timer(slot.entity) : world.add_timer(slot.entity);
        slot.timed = !slot.timed;
      }
      world.frame();
      const auto end = std::chrono::steady_clock::now();
      if (frame >= STEADY_WARM_UP) {
        times.push_back(std::chrono::duration<double, std::nano>(end - start).count());
      }
    }

    std::ranges::sort(times);
    const auto at = [&](size_t percent) { return times[std::min(times.size() - 1, times.size() * percent / 100)]; };
    return {.median = at(50), .percentile90 = at(90), .percentile99 = at(99), .slowest = times.back()};
  }

  // Catch2 only reports the benchmarks it ran itself. These lines on the
  // standard output are what scripts/ecs_compare.py reads instead: the test
  // case, the name of the row and the nanoseconds, separated by tabs.
  inline void report(const char *section, const char *row, double nanoseconds) {
    std::printf("ecs_compare\t%s\t%s\t%.0f\n", section, row, nanoseconds);
    std::fflush(stdout);
  }
  inline void report(const SteadyTimes &times) {
    report(STEADY_WORLD, "frame, median", times.median);
    report(STEADY_WORLD, "frame, 90th percentile", times.percentile90);
    report(STEADY_WORLD, "frame, 99th percentile", times.percentile99);
    report(STEADY_WORLD, "frame, slowest", times.slowest);
  }

} // namespace compare
