#pragma once

#include <catch2/benchmark/catch_benchmark.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <mimalloc.h>

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

  // The components above are a few bytes each. These are not: moving one costs
  // something, and fewer of them fit into a cache line.

  // 64 bytes, all of which a system works on.
  struct Transform {
    std::array<float, 16> matrix;
  };

  // 256 bytes, of which a system wants only a few.
  struct Body {
    float mass;
    std::array<float, 63> state;
  };

  static_assert(sizeof(Transform) == 64 && sizeof(Body) == 256);

  // The work of the systems, the same for every library.
  inline void nudge(Position &pos) { pos.x += DT; }
  inline void heal(Health &health) { health.value = std::min(100.0f, health.value + DT); }
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

  // Reads and writes every byte of the component.
  inline void spin(Transform &transform) {
    for (float &value : transform.matrix) {
      value = value * 0.5f + DT;
    }
  }
  // 4 bytes of 256.
  inline void weigh(Body &body) { body.mass += DT; }
  // A few bytes of each of the two.
  inline void drop(Transform &transform, const Body &body) { transform.matrix[13] -= body.mass * DT; }

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
    // The same with what a library has for making many entities in one go,
    // all of them with the same values.
    inline constexpr const char *CREATE_BATCH = "create entities with 2 components, all at once";
    // The two above in a world that had as many entities and destroyed them
    // all, outside the timing: what the world keeps for its entities has been
    // used, and the indices come back from destroy. The two above fill a new
    // world, with every first use of what it keeps in the timing.
    inline constexpr const char *CREATE_AGAIN = "create entities with 2 components, in a world emptied before";
    inline constexpr const char *CREATE_BATCH_AGAIN =
        "create entities with 2 components, all at once, in a world emptied before";
    inline constexpr const char *DESTROY = "destroy entities";
    // The same with what a library has for destroying many entities in one go.
    inline constexpr const char *DESTROY_BATCH = "destroy entities, all at once";
    inline constexpr const char *ADD_REMOVE = "add + remove a component";
    // The same recorded with Commands and done once the system is over.
    inline constexpr const char *ADD_REMOVE_RECORDED = "add + remove a component, recorded";
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
    // The same entities damage the Health of others: by 1 each, which must
    // all arrive when several threads damage the same one. So far only
    // sc::ecs has a means for that (modify); WRITE_RANDOM is the same
    // without a lock, which only one thread may do.
    inline constexpr const char *WRITE_RANDOM = "1 system, damages its target, no lock";
    inline constexpr const char *MODIFY_RANDOM = "1 system, damages its target";
    inline constexpr const char *MODIFY_FEW = "1 system, damages 1 of 1 000 entities";
    inline constexpr const char *MODIFY_ONE = "1 system, damages the same entity";
    // The same recorded and done once the system is over, one change after
    // the other: sc::ecs Commands::modify.
    inline constexpr const char *RECORD_RANDOM = "1 system, damages its target, recorded";
    inline constexpr const char *RECORD_FEW = "1 system, damages 1 of 1 000 entities, recorded";
    inline constexpr const char *RECORD_ONE = "1 system, damages the same entity, recorded";
    // Not the ECS: every entity adds 1 to one std::atomic, what the hardware
    // takes at least when all threads go for the same cache line.
    inline constexpr const char *ATOMIC_ONE = "1 system, adds to the same atomic";

    // ENTITY_COUNT entities with Transform and Body, see run_heavy.
    inline constexpr const char *CREATE_HEAVY = "create entities with 2 components";
    // Health, 4 bytes. Where a library keeps the components of an entity
    // together, the large ones move along.
    inline constexpr const char *ADD_REMOVE_HEAVY = "add + remove a small component";
    inline constexpr const char *ITERATE_TRANSFORM = "iterate 64 bytes, all of them used";
    inline constexpr const char *ITERATE_BODY = "iterate 256 bytes, 4 of them used";
    inline constexpr const char *ITERATE_HEAVY = "iterate both, 4 bytes of each used";

    // The worlds of SETS_AFTER_WORLD and SETS_WITH_WORLD: the first set (a
    // third of the entities), the second one by its own component, and the
    // component all of them have.
    inline constexpr const char *ITERATE_SET_1 = "iterate the 1st set, Position and Velocity";
    inline constexpr const char *ITERATE_SET_2 = "iterate the 2nd set, Health";
    inline constexpr const char *ITERATE_SETS_ALL = "iterate the component of all 3 sets, Position";
  } // namespace name

  // Names of the test cases, the sections of the report. They state
  // ENTITY_COUNT and PARALLEL_ENTITY_COUNT.
  inline constexpr const char *SMALL_WORLD = "100 000 entities";
  inline constexpr const char *LARGE_WORLD = "1 000 000 entities";
  // ENTITY_COUNT and STEADY_CHANGES.
  inline constexpr const char *STEADY_WORLD = "100 000 entities in use, 1 000 replaced and 1 000 changed per frame";

  inline constexpr const char *HEAVY_WORLD = "100 000 entities with components of 64 and 256 bytes";

  // ENTITY_COUNT entities in three sets of components, created in turn:
  // Position and Velocity, Position and Health, Position alone. Given their
  // components after create, the way every library can, or created with
  // them, with what a library has for that (for sc an area per set).
  inline constexpr const char *SETS_AFTER_WORLD = "100 000 entities in 3 sets, given their components after create";
  inline constexpr const char *SETS_WITH_WORLD = "100 000 entities in 3 sets, created with their components";

  // The benchmarks of HEAVY_WORLD on a world of a library, which is a type
  // with
  //   Handle                     an entity
  //   Handle spawn()             an entity with Transform and Body
  //   void spawn_all(size_t n)   so many of them in one go, if the library has a means for it
  //   void add_health(Handle)    adds Health, which the entity does not have
  //   void remove_health(Handle)
  //   void spin()                spin for every entity
  //   void weigh()               weigh for every entity
  //   void drop()                drop for every entity
  // All of them on one thread.
  template <typename World> void run_heavy() {
    BENCHMARK_ADVANCED(name::serial(name::CREATE_HEAVY))(Catch::Benchmark::Chronometer meter) {
      auto worlds = make_worlds<World>(meter.runs());
      meter.measure([&](int run) {
        World &world = *worlds[static_cast<size_t>(run)];
        for (size_t i = 0; i < ENTITY_COUNT; ++i) {
          world.spawn();
        }
      });
    };

    if constexpr (requires(World &world) { world.spawn_all(ENTITY_COUNT); }) {
      BENCHMARK_ADVANCED(name::serial(name::CREATE_BATCH))(Catch::Benchmark::Chronometer meter) {
        auto worlds = make_worlds<World>(meter.runs());
        meter.measure([&](int run) { worlds[static_cast<size_t>(run)]->spawn_all(ENTITY_COUNT); });
      };
    }

    World world;
    std::vector<typename World::Handle> entities;
    entities.reserve(ENTITY_COUNT);
    for (size_t i = 0; i < ENTITY_COUNT; ++i) {
      entities.push_back(world.spawn());
    }
    BENCHMARK(name::serial(name::ADD_REMOVE_HEAVY)) {
      for (const auto &entity : entities) {
        world.add_health(entity);
      }
      for (const auto &entity : entities) {
        world.remove_health(entity);
      }
    };
    BENCHMARK(name::serial(name::ITERATE_TRANSFORM)) { world.spin(); };
    BENCHMARK(name::serial(name::ITERATE_BODY)) { world.weigh(); };
    BENCHMARK(name::serial(name::ITERATE_HEAVY)) { world.drop(); };
  }

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

  // What a world costs in memory rather than in time. The section of the
  // report; scripts/ecs_compare.py takes the values of a section whose name
  // starts with "memory" as bytes.
  inline constexpr const char *MEMORY = "memory of a world";

  // The rows of the section, each of them a test case of its own: the script
  // runs every one in a new process, so that no world is built in memory an
  // earlier one has left behind. They are hidden from the run of the
  // benchmarks and must not contain what a test spec of Catch2 gives a
  // meaning, a comma above all.
  namespace memory {
    inline constexpr const char *MOVING = "100 000 entities with 2 components";
    inline constexpr const char *FULL = "100 000 entities with 6 components";
    inline constexpr const char *FRAGMENTED = "100 000 entities with 2 to 10 components in 256 combinations";
    inline constexpr const char *LARGE = "1 000 000 entities with 6 components";
    // The world of HEAVY_WORLD.
    inline constexpr const char *HEAVY = "100 000 entities with 2 components of 64 and 256 bytes";
    // The world of STEADY_WORLD after all of its frames.
    inline constexpr const char *STEADY = "100 000 entities in use after 2 200 frames";
    // The worlds of SETS_AFTER_WORLD and SETS_WITH_WORLD.
    inline constexpr const char *SETS_AFTER = "100 000 entities in 3 sets given their components after create";
    inline constexpr const char *SETS_WITH = "100 000 entities in 3 sets created with their components";
  } // namespace memory

  // The bytes of the process that are in memory right now. Counts whatever a
  // library takes, from the allocator or from the OS itself, and only where
  // it was touched: a reserved range of addresses costs nothing here.
  inline size_t resident_memory() {
#ifdef __linux__
    // Counted page by page on request, unlike the numbers of /proc/self/statm,
    // which the kernel only keeps roughly up to date.
    size_t kilobytes = 0;
    if (std::FILE *file = std::fopen("/proc/self/smaps_rollup", "r")) {
      char line[256];
      while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::sscanf(line, "Rss: %zu kB", &kilobytes) == 1) {
          break;
        }
      }
      std::fclose(file);
    }
    return kilobytes * 1024;
#else
    size_t resident = 0;
    mi_process_info(nullptr, nullptr, nullptr, &resident, nullptr, nullptr, nullptr, nullptr);
    return resident;
#endif
  }

  // Reports by how much the memory of the process grows with the world that
  // the function builds and returns. What was freed on the way goes back to
  // the OS first, so that it does not count.
  template <typename Build> void report_memory(const char *row, Build &&build) {
    mi_collect(true);
    const size_t before = resident_memory();
    const auto world = build();
    mi_collect(true);
    const size_t after = resident_memory();
    report(MEMORY, row, after > before ? static_cast<double>(after - before) : 0.0);
  }

  // A world of a library as the benchmarks build it, filled by one of its
  // fill functions. The handles the benchmarks keep are not the library's and
  // are dropped before the memory is read.
  template <typename World, typename Fill> void report_memory(const char *row, size_t count, Fill &&fill) {
    report_memory(row, [&] {
      auto world = std::make_unique<World>(count);
      std::invoke(fill, *world);
      world->entities = {};
      return world;
    });
  }

  // The world of run_heavy, of which 32 MB are the components themselves.
  template <typename World> void report_heavy_memory() {
    report_memory(memory::HEAVY, [] {
      auto world = std::make_unique<World>();
      for (size_t i = 0; i < ENTITY_COUNT; ++i) {
        world->spawn();
      }
      return world;
    });
  }

  // The world of run_steady once it has been in use: what the entities that
  // came and went have left behind counts as well.
  template <typename Steady> void report_steady_memory() {
    report_memory(memory::STEADY, [] {
      auto steady = std::make_unique<Steady>();
      run_steady(*steady);
      return steady;
    });
  }

} // namespace compare
