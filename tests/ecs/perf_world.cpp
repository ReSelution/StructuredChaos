// The direct World paths of ecs_compare on their own, on the calling thread,
// to count their instructions, which do not depend on where the code lands in
// the binary as times do (a few percent from build to build):
//
//   perf_ecs_world <case> <frames>
//
// Cases: create (100K entities made with 2 components and destroyed one by
// one), addremove (a component added to 100K entities and removed again),
// addremove5 (5 components added to each of them in one loop and removed in
// another), get (one component of 100K entities, in random order), iterate
// (10 loops over 100K entities with 3 components) and modify (1M entities
// each modify a random other one). Prints the median frame.
//
// addremove5: past 3 components in one loop, GCC and Clang stopped inlining
// Column::emplace and erase. Forced inline, with World::emplace and erase
// around them, Clang took 52 instead of 73M instructions here but only 4 %
// less time, and GCC 6 % more instructions with 3 components; forced in
// column.hpp alone, the cases with 1 and 3 got worse. So they are not forced.
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
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <optional>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "sc/ecs/world.hpp"
#include "sc/util/macros.hpp"

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
  struct Energy {
    float value;
  };
  struct Score {
    float value;
  };
  struct Armor {
    float value;
  };
  struct Mana {
    float value;
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

  using Entities = std::vector<Entity>;
  using Order = std::vector<size_t>;

  // One frame of each case, each in a function of its own, as the loops of a
  // system are in its lambda: with all cases in run, Clang was short of
  // registers there and loaded the data and the end of the vectors from the
  // stack in every get. Returns what get read, so that it is not optimized
  // away.
  SC_NOINLINE float createFrame(sc::ecs::World &world, Entities &entities, const Order & /*order*/) {
    for (Entity &entity: entities) {
      entity = world.create(Position{.x = 1.0f, .y = 2.0f}, Velocity{.x = 3.0f, .y = 4.0f});
    }
    for (const Entity entity: entities) {
      world.destroy(entity);
    }
    return 0.0f;
  }

  SC_NOINLINE float addRemoveFrame(sc::ecs::World &world, Entities &entities, const Order & /*order*/) {
    for (const Entity entity: entities) {
      world.emplace<Health>(entity, 50.0f);
    }
    for (const Entity entity: entities) {
      world.erase<Health>(entity);
    }
    return 0.0f;
  }

  SC_NOINLINE float addRemove5Frame(sc::ecs::World &world, Entities &entities, const Order & /*order*/) {
    for (const Entity entity: entities) {
      world.emplace<Health>(entity, 50.0f);
      world.emplace<Energy>(entity, 1.0f);
      world.emplace<Score>(entity, 0.0f);
      world.emplace<Armor>(entity, 2.0f);
      world.emplace<Mana>(entity, 3.0f);
    }
    for (const Entity entity: entities) {
      world.erase<Health>(entity);
      world.erase<Energy>(entity);
      world.erase<Score>(entity);
      world.erase<Armor>(entity);
      world.erase<Mana>(entity);
    }
    return 0.0f;
  }

  SC_NOINLINE float getFrame(sc::ecs::World &world, Entities &entities, const Order &order) {
    float sum = 0.0f;
    for (const size_t i: order) {
      sum += world.get<const Position>(entities[i]).x;
    }
    return sum;
  }

  SC_NOINLINE float iterateFrame(sc::ecs::World &world, Entities & /*entities*/, const Order & /*order*/) {
    for (int loop = 0; loop < 10; ++loop) {
      world.each<Position, const Velocity, const Timer>(
          [](Position &position, const Velocity &velocity, const Timer &) {
            position.x += velocity.x;
            position.y += velocity.y;
          });
    }
    return 0.0f;
  }

  SC_NOINLINE float modifyFrame(sc::ecs::World &world, Entities & /*entities*/, const Order & /*order*/) {
    world.each<const Target>([&](const Target &target) {
      world.modify<Health>(target.entity, [](Health &health) { health.value -= 1.0f; });
    });
    return 0.0f;
  }

  using Frame = float (*)(sc::ecs::World &, Entities &, const Order &);

  struct Case {
    std::string_view name;
    Frame frame;
  };

  constexpr std::array CASES{
      Case{.name = "create", .frame = createFrame},         Case{.name = "addremove", .frame = addRemoveFrame},
      Case{.name = "addremove5", .frame = addRemove5Frame}, Case{.name = "get", .frame = getFrame},
      Case{.name = "iterate", .frame = iterateFrame},       Case{.name = "modify", .frame = modifyFrame},
  };

  void run(const Case &which, uint32_t frames) {
    // The calling thread alone, with thread id 0.
    sc::threading::init(0);

    const bool creating = which.name == "create";
    const bool targets = which.name == "modify" || which.name == "get";
    const size_t count = which.name == "modify" ? 1'000'000 : 100'000;
    sc::ecs::World world;
    Entities entities(count);
    Order order(count);
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

    // Printed once the frames are over, so that get is not optimized away.
    float sum = 0.0f;
    std::vector<double> times;
    for (uint32_t frame = 0; frame < frames; ++frame) {
      const auto start = std::chrono::steady_clock::now();
      sum += which.frame(world, entities, order);
      times.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    }
    if (!times.empty()) {
      std::ranges::sort(times);
      std::println("{}: median frame {:.1f} us (sum {})", which.name, times[times.size() / 2], sum);
    }
  }

} // namespace

int main(int argc, char **argv) {
  const std::optional<uint32_t> frames = argc == 3 ? number(argv[2]) : std::nullopt;
  const auto *const which = argc == 3 ? std::ranges::find(CASES, std::string_view{argv[1]}, &Case::name) : CASES.end();
  if (!frames || which == CASES.end()) {
    std::fputs("usage: perf_ecs_world create|addremove|addremove5|get|iterate|modify <frames>\n", stderr);
    return 1;
  }
  try {
    run(*which, *frames);
  } catch (const std::exception &error) {
    std::fputs(error.what(), stderr);
    std::fputs("\n", stderr);
    return 1;
  }
  return 0;
}
