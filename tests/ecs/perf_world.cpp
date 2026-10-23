// The direct World paths of ecs_compare on their own, on the calling thread,
// to count their instructions, which do not depend on where the code lands in
// the binary as times do (a few percent from build to build):
//
//   perf_ecs_world <case> <frames>
//
// Cases: create (100K entities made with 2 components and destroyed one by
// one), addremove (a component added to 100K entities and removed again),
// get (one component of 100K entities, in random order), iterate (10 loops
// over 100K entities with 3 components) and modify (1M entities each modify
// a random other one). Prints the median frame.
//
// Instructions per frame, from two frame counts so that setting up drops out:
//
//   perf stat -x, -e instructions:u perf_ecs_world create 3
//   perf stat -x, -e instructions:u perf_ecs_world create 23
//   (second - first) / 20
//
// Built for this when forcing a function of World inline: measure again after
// changing it or updating a compiler, see the comments there.

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

#include "sc/ecs/world.hpp"

namespace {

  using sc::ecs::Entity;

  struct Position {
    float x, y;
  };
  struct Velocity {
    float x, y;
  };
  struct Health {
    float value;
  };
  struct Timer {
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

  void run(const std::string &which, uint32_t frames) {
    // The calling thread alone, with thread id 0.
    sc::threading::init(0);

    const bool creating = which == "create";
    const bool targets = which == "modify" || which == "get";
    const size_t count = which == "modify" ? 1'000'000 : 100'000;
    sc::ecs::World world;
    std::vector<Entity> entities(count);
    std::vector<size_t> order(count);
    for (size_t i = 0; i < count; ++i) {
      order[i] = i;
    }
    // The same order in every run, so that the counts compare.
    std::ranges::shuffle(order, std::mt19937_64{42}); // NOLINT(bugprone-random-generator-seed)
    if (!creating) {
      for (Entity &entity: entities) {
        entity = world.create();
      }
      for (size_t i = 0; i < count; ++i) {
        world.emplace<Position>(entities[i], static_cast<float>(i), 1.0f);
        world.emplace<Velocity>(entities[i], 1.0f, 2.0f);
        world.emplace<Timer>(entities[i], 0.0f);
        if (targets) {
          world.emplace<Health>(entities[i], 50.0f);
          world.emplace<Target>(entities[i], entities[order[i]]);
        }
      }
    }

    // Read once the frames are over, so that get is not optimized away.
    float sum = 0.0f;
    std::vector<double> times;
    for (uint32_t frame = 0; frame < frames; ++frame) {
      const auto start = std::chrono::steady_clock::now();
      if (creating) {
        for (Entity &entity: entities) {
          entity = world.create(Position{.x = 1.0f, .y = 2.0f}, Velocity{.x = 3.0f, .y = 4.0f});
        }
        for (const Entity entity: entities) {
          world.destroy(entity);
        }
      } else if (which == "addremove") {
        for (const Entity entity: entities) {
          world.emplace<Health>(entity, 50.0f);
        }
        for (const Entity entity: entities) {
          world.erase<Health>(entity);
        }
      } else if (which == "get") {
        for (const size_t i: order) {
          sum += world.get<const Position>(entities[i]).x;
        }
      } else if (which == "iterate") {
        for (int loop = 0; loop < 10; ++loop) {
          world.each<Position, const Velocity, const Timer>(
              [](Position &position, const Velocity &velocity, const Timer &) {
                position.x += velocity.x;
                position.y += velocity.y;
              });
        }
      } else {
        world.each<const Target>([&](const Target &target) {
          world.modify<Health>(target.entity, [](Health &health) { health.value -= 1.0f; });
        });
      }
      times.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    }
    if (!times.empty()) {
      std::ranges::sort(times);
      std::println("{}: median frame {:.1f} us (sum {})", which, times[times.size() / 2], sum);
    }
  }

} // namespace

int main(int argc, char **argv) {
  const std::vector<std::string> cases{"create", "addremove", "get", "iterate", "modify"};
  const std::optional<uint32_t> frames = argc == 3 ? number(argv[2]) : std::nullopt;
  if (!frames || std::ranges::find(cases, argv[1]) == cases.end()) {
    std::fputs("usage: perf_ecs_world create|addremove|get|iterate|modify <frames>\n", stderr);
    return 1;
  }
  try {
    run(argv[1], *frames);
  } catch (const std::exception &error) {
    std::fputs(error.what(), stderr);
    std::fputs("\n", stderr);
    return 1;
  }
  return 0;
}
