// The Commands cases of ecs_compare on their own, to count the instructions
// of recording and applying changes, which do not depend on where the code
// lands in the binary as times do (a few percent from build to build):
//
//   perf_ecs_commands <case> <frames> <threads>
//
// Cases: serial (1M entities each modify a random other one, recorded on one
// thread), random (the same in a parallel loop), one (all of them the same
// entity, so the changes need no sort) and addremove (100K entities get a
// component and lose it again). With 0 threads the calling thread does it
// all, with a list of its own, which makes the instruction count the same
// from run to run. Prints the median frame.
//
// Instructions per frame, from two frame counts so that setting up drops out:
//
//   perf stat -x, -e instructions:u perf_ecs_commands one 2 0
//   perf stat -x, -e instructions:u perf_ecs_commands one 12 0
//   (second - first) / 10
//
// Built for this when forcing a function inline in CommandApplier: measure
// again after changing it or updating a compiler, see the comments there.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <optional>
#include <print>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "sc/ecs/scheduler.hpp"

namespace {

  using sc::ecs::Commands;
  using sc::ecs::Entity;
  using sc::ecs::Query;
  using sc::ecs::Scheduler;

  struct Position {
    float x, y;
  };
  struct Health {
    float value;
  };
  struct Target {
    Entity entity;
  };

  // The whole text as a number, or none.
  std::optional<uint32_t> number(const char *text) {
    uint32_t value = 0;
    const char *const end = text + std::strlen(text);
    const auto [stop, error] = std::from_chars(text, end, value);
    if (error != std::errc{} || stop != end) {
      return std::nullopt;
    }
    return value;
  }

  void run(const std::string &which, uint32_t frames, uint32_t threads) {
    // With 0 the calling thread alone, with thread id 0 and a list of its
    // own: without init it would share the locked list of the threads
    // outside the pool, and a quarter of the instructions were taking and
    // giving back the lock.
    sc::threading::init(threads);

    const bool adding = which == "addremove";
    const size_t count = adding ? 100'000 : 1'000'000;
    sc::ecs::World world;
    std::vector<Entity> entities(count);
    for (Entity &entity: entities) {
      entity = world.create();
    }
    std::vector<size_t> order(count);
    for (size_t i = 0; i < count; ++i) {
      order[i] = i;
    }
    // The same targets in every run, so that the counts compare.
    std::ranges::shuffle(order, std::mt19937_64{42}); // NOLINT(bugprone-random-generator-seed)
    for (size_t i = 0; i < count; ++i) {
      world.emplace<Position>(entities[i], 1.0f, 2.0f);
      if (!adding) {
        world.emplace<Health>(entities[i], 50.0f);
        world.emplace<Target>(entities[i], entities[order[i]]);
      }
    }

    const auto damage = [](Health &health) { health.value -= 1.0f; };
    const Entity boss = entities.front();
    Scheduler first;
    Scheduler second;
    if (which == "serial") {
      first.add([&](Query<const Target> query, Commands commands) {
        query.each([&](const Target &target) { commands.modify(target.entity, damage); });
      });
    } else if (which == "random") {
      first.addParallel([&](Commands commands, const Target &target) { commands.modify(target.entity, damage); });
    } else if (which == "one") {
      first.addParallel([&](Commands commands, const Target &) { commands.modify(boss, damage); });
    } else {
      first.addParallel(
          [](Entity entity, Commands commands, const Position &) { commands.emplace<Health>(entity, 50.0f); });
      second.addParallel([](Entity entity, Commands commands, const Position &) { commands.erase<Health>(entity); });
    }

    std::vector<double> times;
    for (uint32_t frame = 0; frame < frames; ++frame) {
      const auto start = std::chrono::steady_clock::now();
      first.run(world);
      if (adding) {
        second.run(world);
      }
      times.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    }
    if (!times.empty()) {
      std::ranges::sort(times);
      std::println("{}: median frame {:.1f} us", which, times[times.size() / 2]);
    }
  }

} // namespace

int main(int argc, char **argv) {
  const std::vector<std::string> cases{"serial", "random", "one", "addremove"};
  const std::optional<uint32_t> frames = argc == 4 ? number(argv[2]) : std::nullopt;
  const std::optional<uint32_t> threads = argc == 4 ? number(argv[3]) : std::nullopt;
  if (!frames || !threads || std::ranges::find(cases, argv[1]) == cases.end()) {
    std::fputs("usage: perf_ecs_commands serial|random|one|addremove <frames> <threads>\n", stderr);
    return 1;
  }
  try {
    run(argv[1], *frames, *threads);
  } catch (const std::exception &error) {
    std::fputs(error.what(), stderr);
    std::fputs("\n", stderr);
    return 1;
  }
  return 0;
}
