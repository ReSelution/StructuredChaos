#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "sc/ecs/scheduler.hpp"
#include "sc/ecs/world.hpp"
#include "sc/threading/threading.hpp"

namespace {

  using sc::ecs::Commands;
  using sc::ecs::Edit;
  using sc::ecs::Emit;
  using sc::ecs::Entity;
  using sc::ecs::Exclude;
  using sc::ecs::Global;
  using sc::ecs::LocalEdit;
  using sc::ecs::PAGE_SIZE;
  using sc::ecs::Query;
  using sc::ecs::ResourceCache;
  using sc::ecs::ResourceHandle;
  using sc::ecs::Resources;
  using sc::ecs::Scheduler;
  using sc::ecs::Spawn;
  using sc::ecs::World;

  struct Position {
    float x{0.0f};
    float y{0.0f};
  };

  struct Velocity {
    float dx{0.0f};
    float dy{0.0f};
  };

  struct Health {
    int value{100};
  };

  struct Frozen {};

  struct Spawner {
    int count{0};
  };

  struct Target {
    int value{0};
  };

  // Owns something, which shows whether every one that was made is gone.
  struct Named {
    std::string name;
    std::shared_ptr<int> alive;
  };

  // Cannot be given to an entity while told so.
  struct Fragile {
    static inline bool breaks = false;

    int value{0};

    explicit Fragile(int number) : value(number) {}
    // Throws on purpose: a component whose move fails.
    // NOLINTNEXTLINE(bugprone-exception-escape,performance-noexcept-move-constructor)
    Fragile(Fragile &&other) : value(other.value) {
      if (breaks) {
        throw std::runtime_error("broken");
      }
    }
  };

  // A resource that shows how often it was loaded.
  struct Texture {
    int id;
  };

  struct Material {
    ResourceHandle<Texture> albedo;
  };

  struct Hit {
    Entity target;
    int damage{1};
  };

  struct Died {
    Entity entity;
  };

  // A global value of the world.
  struct GameTime {
    float seconds;
  };

  using Stages = std::vector<std::vector<size_t>>;

  // Edit has no get: it would get past the bit that modify takes.
  template<typename EditType>
  concept HasGet = requires(const EditType &edit, Entity entity) { edit.template get<Health>(entity); };
  static_assert(!HasGet<Edit<Health>>);
  static_assert(HasGet<LocalEdit<Health>>);

  void move(Query<Position, const Velocity> query) {
    query.each([](Position &pos, const Velocity &vel) {
      pos.x += vel.dx;
      pos.y += vel.dy;
    });
  }

  struct Doubler {
    void operator()(const Query<Position> &query) const {
      query.each([](Position &pos) { pos.y *= 2.0f; });
    }
  };

} // namespace

TEST_CASE("ECS Scheduler Stages", "[ecs][scheduler]") {
  Scheduler scheduler;

  SECTION("Nothing To Run") { REQUIRE(scheduler.stages().empty()); }

  SECTION("Readers Of The Same Component Run Together") {
    scheduler.add([](Query<const Position>) {});
    scheduler.add([](Query<const Position, const Velocity>) {});
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
  }

  SECTION("Writers Of Different Components Run Together") {
    scheduler.add([](Query<Position, const Velocity>) {});
    scheduler.add([](Query<Health, const Velocity>) {});
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
  }

  SECTION("A Writer Waits For Earlier Readers And Writers") {
    scheduler.add([](Query<const Position>) {});
    scheduler.add([](Query<Position>) {});
    scheduler.add([](Query<Position>) {});
    scheduler.add([](Query<const Position>) {});
    REQUIRE(scheduler.stages() == Stages{{0}, {1}, {2}, {3}});
  }

  SECTION("A System Only Waits For The Ones It Conflicts With") {
    scheduler.add([](Query<Position>) {});
    scheduler.add([](Query<Position>) {});
    scheduler.add([](Query<Health>) {});
    REQUIRE(scheduler.stages() == Stages{{0, 2}, {1}});
  }

  SECTION("An Excluded Component Counts As Read") {
    scheduler.add([](Query<Position, Exclude<Frozen>>) {});
    scheduler.add([](Query<const Health>, Edit<Frozen>) {});
    scheduler.add([](Query<const Health, Exclude<Frozen>>) {});
    REQUIRE(scheduler.stages() == Stages{{0}, {1}, {2}});
  }

  SECTION("Editing A Component Counts As Writing It") {
    scheduler.add([](Query<const Position>, Edit<Health>) {});
    scheduler.add([](Query<const Health>) {});
    scheduler.add([](Query<const Position>, Edit<Spawner, Target>) {});
    scheduler.addParallel([](Edit<Target>, const Velocity &) {});
    scheduler.add([](Query<const Position>, Edit<Frozen>) {});
    REQUIRE(scheduler.stages() == Stages{{0, 2, 4}, {1, 3}});
  }

  SECTION("Editing Without The Bit Counts As Writing As Well") {
    scheduler.add([](Query<const Position>, LocalEdit<Health>) {});
    scheduler.add([](Query<const Health>) {});
    scheduler.addParallel([](LocalEdit<Spawner>, const Velocity &) {});
    REQUIRE(scheduler.stages() == Stages{{0, 2}, {1}});
  }

  SECTION("A Loop May Go Through What Its System Edits") {
    // The function edits it only on its own entity, see World::each.
    scheduler.addParallel([](Entity, Edit<Health>, Health &) {});
    scheduler.add([](Query<const Health>) {});
    REQUIRE(scheduler.stages() == Stages{{0}, {1}});
  }

  SECTION("Spawning Keeps Nobody Waiting") {
    scheduler.add([](Query<Position>, Spawn) {});
    scheduler.add([](Spawn) {});
    scheduler.addParallel([](Spawn, Position &) {});
    REQUIRE(scheduler.stages() == Stages{{0, 1}, {2}});
  }

  SECTION("Commands Keep Nobody Waiting") {
    scheduler.add([](Query<const Position>, Commands) {});
    scheduler.add([](Query<const Position>, Commands) {});
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
  }

  SECTION("A Cache Keeps Nobody Waiting") {
    scheduler.add([](Query<Health>, Resources<Texture>) {});
    scheduler.add([](Resources<Texture>) {});
    scheduler.addParallel([](Resources<Texture>, Health &) {});
    REQUIRE(scheduler.stages() == Stages{{0, 1}, {2}});
  }

  SECTION("An Emitter Uses What Its Listeners Use") {
    scheduler.add([](Query<const Position>, Emit<Hit>) {});
    scheduler.add([](Query<const Health>) {});
    scheduler.add([](Query<const Position>) {});
    scheduler.add([](Query<const Frozen>) {});
    // Added after the system that emits the event, and through another event.
    scheduler.on([](const Hit &, Edit<Health>, Emit<Died>) {});
    scheduler.on([](Died, Edit<Frozen>) {});
    REQUIRE(scheduler.stages() == Stages{{0, 2}, {1, 3}});
  }

  SECTION("An Event Without Listeners Keeps Nobody Waiting") {
    scheduler.add([](Query<const Position>, Emit<Hit>) {});
    scheduler.add([](Query<Health>) {});
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
  }

  SECTION("A Listener With Nothing But The Event Makes Its Emitter Run Alone") {
    scheduler.add([](Query<const Position>, Emit<Hit>) {});
    scheduler.add([](Query<const Health>) {});
    scheduler.on([](const Hit &) {});
    REQUIRE(scheduler.stages() == Stages{{0}, {1}});
  }

  SECTION("Events That Emit Each Other") {
    scheduler.add([](Query<const Position>, Emit<Hit>) {});
    scheduler.add([](Query<const Health>) {});
    scheduler.on([](const Hit &, Emit<Died>) {});
    scheduler.on([](const Died &, Emit<Hit>, Edit<Health>) {});
    REQUIRE(scheduler.stages() == Stages{{0}, {1}});
  }

  SECTION("A Listener Must Not Edit What Its Emitter Queries") {
    scheduler.addParallel([](Emit<Hit>, Query<const Health>, const Position &) {}, "fight");
    scheduler.on([](const Hit &, Edit<Health>) {});
    REQUIRE_THROWS_AS(scheduler.stages(), std::logic_error);
  }

  SECTION("A Listener May Edit What Its Emitter's Loop Goes Through") {
    scheduler.addParallel([](Emit<Hit>, Health &) {}, "fight");
    scheduler.on([](const Hit &, Edit<Health>) {});
    REQUIRE(scheduler.stages() == Stages{{0}});
  }

  SECTION("A System That Takes The World Or Nothing Runs Alone") {
    scheduler.add([](Query<const Position>) {});
    scheduler.add([](World &) {}, "load");
    scheduler.add([](Query<const Health>) {});
    scheduler.add([] {});
    scheduler.add([](Query<const Health>) {});
    REQUIRE(scheduler.stages() == Stages{{0}, {1}, {2}, {3}, {4}});
    REQUIRE(scheduler.name(1) == "load");
    REQUIRE(scheduler.size() == 5);
  }

  SECTION("Readers Of A Global Run Together, A Writer Alone") {
    scheduler.add([](Global<const GameTime>) {});
    scheduler.add([](Global<const GameTime>, Query<Position>) {});
    scheduler.add([](Global<GameTime>) {});
    scheduler.add([](Global<const GameTime>) {});
    REQUIRE(scheduler.stages() == Stages{{0, 1}, {2}, {3}});
  }

  SECTION("A Global And A Component Of The Same Type Are Apart") {
    scheduler.add([](Global<Health>) {});
    scheduler.add([](Query<Health>) {});
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
  }

  SECTION("A Loop Reads A Global") {
    scheduler.addParallel([](Global<const GameTime>, Position &) {});
    scheduler.add([](Global<GameTime>) {});
    scheduler.addParallel([](Global<const GameTime>, Health &) {});
    REQUIRE(scheduler.stages() == Stages{{0}, {1}, {2}});
  }

  SECTION("A Listener Writes A Global For A System, Not For A Loop") {
    scheduler.add([](Query<const Position>, Emit<Hit>) {}, "fight");
    scheduler.add([](Global<const GameTime>) {});
    scheduler.on([](const Hit &, Global<GameTime>) {});
    REQUIRE(scheduler.stages() == Stages{{0}, {1}});
    scheduler.addParallel([](Emit<Hit>, const Position &) {}, "fight on all threads");
    REQUIRE_THROWS_AS(scheduler.stages(), std::logic_error);
  }

  SECTION("A System Counts As Using What The Hooks Of What It Edits Use") {
    World world;
    // Health sets off Spawner, which sets off Target.
    world.onAdd<Health>([](Entity, Health &, Edit<Spawner>) {});
    world.onAdd<Spawner>([](Entity, Spawner &, Edit<Target>) {});
    scheduler.add([](Edit<Health>) {});
    scheduler.add([](Query<const Target>) {});
    scheduler.add([](Commands) {});
    scheduler.add([](Query<const Target>) {});
    // Without the world, and for Commands, whose hooks run after the stage.
    REQUIRE(scheduler.stages() == Stages{{0, 1, 2, 3}});
    REQUIRE(scheduler.stages(world) == Stages{{0, 2}, {1, 3}});
  }

  SECTION("A Hook Must Not Edit What Its System Takes Through LocalEdit") {
    World world;
    world.onAdd<Spawner>([](Entity, Spawner &, Edit<Health>) {});
    scheduler.addParallel([](LocalEdit<Spawner, Health>, const Position &) {}, "spawn");
    REQUIRE(scheduler.stages() == Stages{{0}});
    REQUIRE_THROWS_AS(scheduler.stages(world), std::logic_error);
  }

  SECTION("Hooks Must Not Edit What Other Hooks Query") {
    World world;
    world.onAdd<Spawner>([](Entity, Spawner &, Edit<Health>) {});
    world.onAdd<Target>([](Entity, Target &, Query<const Health>) {});
    REQUIRE_THROWS_AS(scheduler.stages(world), std::logic_error);
  }

  SECTION("The Parameters Of A Loop Say What It Uses") {
    scheduler.addParallel([](Position &, const Velocity &) {});
    scheduler.addParallel([](Health &, Velocity) {});
    scheduler.addParallel([](Entity, Edit<Frozen>, const Health &) {});
    scheduler.addParallel<Query<Velocity, Exclude<Frozen>>>([](Velocity &) {});
    REQUIRE(scheduler.stages() == Stages{{0, 1}, {2}, {3}});
  }
}

TEST_CASE("ECS Scheduler Runs Systems", "[ecs][scheduler][threading]") {
  sc::threading::init();

  World world;
  std::vector<Entity> entities;
  for (size_t i = 0; i < (2 * PAGE_SIZE) + 300; ++i) {
    entities.push_back(world.create());
    world.emplace<Position>(entities.back(), static_cast<float>(i), 0.0f);
    world.emplace<Health>(entities.back());
    if (i % 3 != 0) {
      world.emplace<Velocity>(entities.back(), 1.0f, 2.0f);
    }
  }

  Scheduler scheduler;

  SECTION("A Query Looks Up Components Of Entities That May Not Have Them") {
    // Every third entity has no Velocity.
    size_t without = 0;
    scheduler.add([&](Query<const Velocity> query) {
      for (const Entity entity: entities) {
        const Velocity *const vel = query.tryGet<const Velocity>(entity);
        without += vel == nullptr ? 1 : 0;
      }
    });
    scheduler.run(world);
    REQUIRE(without == (entities.size() + 2) / 3);
  }

  SECTION("Functions, Function Objects And Lambdas") {
    scheduler.add(move);
    scheduler.add(Doubler{});
    scheduler.add([](Query<Health> query) { query.each([](Health &health) { health.value += 1; }); });
    REQUIRE(scheduler.stages() == Stages{{0, 2}, {1}});
    scheduler.run(world);
    scheduler.run(world);
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.get<Position>(entities[i]).y == (i % 3 != 0 ? 12.0f : 0.0f));
      REQUIRE(world.get<Health>(entities[i]).value == 102);
    }
  }

  SECTION("Loops On Several Threads") {
    scheduler.addParallel([](Position &pos, const Velocity &vel) { pos.y += vel.dy; }, "move");
    scheduler.addParallel([](Entity entity, Health &health, Velocity vel) {
      health.value += static_cast<int>(vel.dx) + static_cast<int>(sc::ecs::indexOf(entity) % 2);
    });
    scheduler.addParallel<Query<Position, Exclude<Velocity>>>([](Position &pos) { pos.y -= 1.0f; });
    REQUIRE(scheduler.stages() == Stages{{0, 1}, {2}});
    scheduler.run(world);
    for (size_t i = 0; i < entities.size(); ++i) {
      const bool moving = i % 3 != 0;
      REQUIRE(world.get<Position>(entities[i]).y == (moving ? 2.0f : -1.0f));
      REQUIRE(world.get<Health>(entities[i]).value ==
              (moving ? 101 + static_cast<int>(sc::ecs::indexOf(entities[i]) % 2) : 100));
    }
  }

  SECTION("A Loop Adds And Removes Components Of Its Entity") {
    scheduler.addParallel([](Entity entity, Edit<Frozen, Velocity> edit, const Position &pos) {
      const auto i = static_cast<size_t>(pos.x);
      if (i % 2 == 0) {
        edit.emplace<Frozen>(entity);
      }
      if (i % 3 == 1) {
        edit.erase<Velocity>(entity);
      }
    });
    // Sees what the loop did.
    std::atomic<size_t> frozen = 0;
    scheduler.addParallel<Query<const Frozen, const Position>>([&](const Position &) { ++frozen; });
    REQUIRE(scheduler.stages() == Stages{{0}, {1}});
    scheduler.run(world);

    REQUIRE(frozen == (entities.size() + 1) / 2);
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.contains<Frozen>(entities[i]) == (i % 2 == 0));
      REQUIRE(world.contains<Velocity>(entities[i]) == (i % 3 == 2));
    }
  }

  SECTION("Entities Are Destroyed Once The Stage Is Over") {
    std::atomic<size_t> seen = 0;
    scheduler.addParallel([](Entity entity, Commands commands, const Position &pos) {
      if (static_cast<size_t>(pos.x) % 2 == 0) {
        commands.destroy(entity);
        // Twice does no harm.
        commands.destroy(entity);
      }
    });
    // Next to it, so every entity is still there.
    scheduler.addParallel([&](const Health &) { ++seen; });
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
    scheduler.run(world);

    REQUIRE(seen == entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.valid(entities[i]) == (i % 2 != 0));
    }
    size_t left = 0;
    world.each<const Position>([&](const Position &) { ++left; });
    REQUIRE(left == entities.size() / 2);

    // Nothing is left over for the next run.
    seen = 0;
    scheduler.run(world);
    REQUIRE(seen == entities.size() / 2);
  }

  SECTION("Threads Create Entities With Components") {
    constexpr int perSpawner = 500;
    for (size_t i = 0; i < entities.size(); i += 100) {
      world.emplace<Spawner>(entities[i], perSpawner);
    }
    const size_t spawners = (entities.size() + 99) / 100;
    // Changes its own entity and the ones it creates, which must not get in
    // each other's way.
    scheduler.addParallel([](Entity entity, Edit<Frozen, Velocity> edit, const Position &) {
      if (edit.contains<Frozen>(entity)) {
        edit.erase<Frozen>(entity);
      } else {
        edit.emplace<Frozen>(entity);
      }
    });
    scheduler.addParallel([](Edit<Frozen, Velocity> edit, Spawn spawn, const Spawner &spawner) {
      for (int i = 0; i < spawner.count; ++i) {
        const Entity spawned = spawn.create();
        edit.emplace<Frozen>(spawned);
        edit.emplace<Velocity>(spawned, 7.0f, 7.0f);
      }
    });
    // Twice: back to what it was for the first loop.
    scheduler.addParallel([](Entity entity, Edit<Frozen> edit, const Position &) {
      if (edit.contains<Frozen>(entity)) {
        edit.erase<Frozen>(entity);
      } else {
        edit.emplace<Frozen>(entity);
      }
    });
    REQUIRE(scheduler.stages() == Stages{{0}, {1}, {2}});
    const size_t before = (entities.size() - ((entities.size() + 2) / 3));
    scheduler.run(world);

    size_t spawned = 0;
    world.each<const Frozen, const Velocity>(
        [&](Entity entity, const Velocity &vel) { spawned += world.valid(entity) && vel.dx == 7.0f; });
    REQUIRE(spawned == spawners * perSpawner);
    size_t velocities = 0;
    world.each<const Velocity>([&](const Velocity &) { ++velocities; });
    REQUIRE(velocities == before + (spawners * perSpawner));
  }

  SECTION("Systems Spawn Entities In The Areas Of Their Sets") {
    // Every entity with a velocity spawns three: one with its components
    // right away, one created for them and given them after, and one
    // through Commands.
    scheduler.addParallel([](Edit<Frozen, Spawner> edit, Spawn spawn, const Velocity &) {
      (void) edit.create(Frozen{}, Spawner{1});
      const Entity planned = spawn.create<Spawner, Frozen>();
      edit.emplace<Frozen>(planned);
      edit.emplace<Spawner>(planned, 2);
    });
    scheduler.addParallel([](Commands commands, const Velocity &) {
      const Entity made = commands.create<Target>();
      commands.emplace<Target>(made, 3);
    });
    scheduler.run(world);

    size_t moving = 0;
    world.each<const Velocity>([&](const Velocity &) { ++moving; });
    // Which set gets its area first depends on which thread gets there
    // first: the two systems run side by side. So per set the pages it
    // has, which must be a few near each other, apart from the other set's
    // and from the pages of the entities before.
    const auto pageOf = [](Entity entity) { return sc::ecs::indexOf(entity) >> sc::ecs::PAGE_BITS; };
    std::vector<uint32_t> spawnerPages;
    std::vector<uint32_t> targetPages;
    size_t right = 0;
    world.each<const Frozen, const Spawner>([&](Entity entity, const Spawner &spawner) {
      spawnerPages.push_back(pageOf(entity));
      right += spawner.count == 1 || spawner.count == 2;
    });
    world.each<const Target>([&](Entity entity, const Target &target) {
      targetPages.push_back(pageOf(entity));
      right += target.value == 3;
    });
    REQUIRE(spawnerPages.size() == 2 * moving);
    REQUIRE(targetPages.size() == moving);
    REQUIRE(right == 3 * moving);
    for (std::vector<uint32_t> *pages: {&spawnerPages, &targetPages}) {
      std::ranges::sort(*pages);
      // A page per thread at most beyond those the entities fill.
      REQUIRE(pages->back() - pages->front() < 4 + sc::threading::threadSlots());
      REQUIRE(pages->front() >= 1u << 17);
    }
    REQUIRE((spawnerPages.back() < targetPages.front() || targetPages.back() < spawnerPages.front()));
  }

  SECTION("Creating And Destroying Run After Run Does Not Use Up The Indices") {
    for (size_t i = 0; i < entities.size(); i += 100) {
      world.emplace<Spawner>(entities[i], 3);
    }
    const size_t spawners = (entities.size() + 99) / 100;
    // Every run destroys what the one before created.
    scheduler.addParallel<Query<const Frozen>>([](Entity entity, Commands commands) { commands.destroy(entity); });
    scheduler.addParallel([](Edit<Frozen> edit, Spawn spawn, const Spawner &spawner) {
      for (int i = 0; i < spawner.count; ++i) {
        edit.emplace<Frozen>(spawn.create());
      }
    });
    REQUIRE(scheduler.stages() == Stages{{0}, {1}});

    // Far more runs than the pages below: an index lost per run would show
    // as more pages. Not more runs than there are pages, which with 2^20
    // of them took minutes.
    const size_t runs = 8192;
    for (size_t run = 0; run < runs; ++run) {
      scheduler.run(world);
    }
    size_t spawned = 0;
    world.each<const Frozen>([&](Entity entity) { spawned += world.valid(entity); });
    REQUIRE(spawned == spawners * 3);
    // A page for every thread at most, next to the first ones: after that
    // the indices of the destroyed entities do.
    REQUIRE(world.pageCount() <= 3 + sc::threading::threadSlots());
    for (const Entity entity: entities) {
      REQUIRE(world.valid(entity));
    }
  }

  SECTION("A Loop Changes Its Entity Next To Entities Created In The Same Page") {
    // Indices all over the pages for the new entities to get.
    for (size_t i = 1; i < entities.size(); i += 2) {
      world.destroy(entities[i]);
    }
    const size_t left = (entities.size() + 1) / 2;
    const uint32_t pages = world.pageCount();
    // Every thread adds the same components to its own entity and to the one
    // it creates, which is in the page of some thread or other.
    scheduler.addParallel([](Entity entity, Edit<Frozen, Spawner> edit, Spawn spawn, const Position &pos) {
      edit.emplace<Frozen>(entity);
      edit.emplace<Spawner>(entity, static_cast<int>(pos.x));
      const Entity spawned = spawn.create();
      edit.emplace<Frozen>(spawned);
      edit.emplace<Spawner>(spawned, -1);
    });
    // And takes them away again, the last ones of a page among them.
    scheduler.addParallel([](Entity entity, Edit<Frozen, Spawner> edit, const Position &) {
      edit.erase<Frozen>(entity);
      edit.erase<Spawner>(entity);
    });
    REQUIRE(scheduler.stages() == Stages{{0}, {1}});
    scheduler.run(world);

    REQUIRE(world.pageCount() == pages);
    size_t created = 0;
    world.each<const Frozen, const Spawner>([&](Entity entity, const Spawner &spawner) {
      created += world.valid(entity) && !world.contains<Position>(entity) && spawner.count == -1;
    });
    REQUIRE(created == left);
    size_t frozen = 0;
    world.each<const Frozen>([&](Entity) { ++frozen; });
    REQUIRE(frozen == left);
    for (size_t i = 0; i < entities.size(); i += 2) {
      REQUIRE_FALSE(world.contains<Spawner>(entities[i]));
    }
  }

  SECTION("A Loop Changes Entities That Other Threads Go For As Well") {
    // Every entity hits one of a few, as do many on other threads.
    constexpr size_t targets = 10;
    scheduler.addParallel([&](Edit<Spawner, Frozen> edit, const Position &pos) {
      const Entity target = entities[static_cast<size_t>(pos.x) % targets];
      edit.tryEmplace<Spawner>(target, 5);
      edit.tryEmplace<Frozen>(target);
    });
    std::atomic<size_t> thawed = 0;
    scheduler.addParallel([&](Edit<Frozen> edit, const Position &pos) {
      thawed += edit.erase<Frozen>(entities[static_cast<size_t>(pos.x) % targets]);
    });
    REQUIRE(scheduler.stages() == Stages{{0}, {1}});
    scheduler.run(world);

    REQUIRE(world.column<Spawner>().page(0).count == targets);
    REQUIRE(thawed == targets);
    REQUIRE(world.column<Frozen>().pages() == 0);
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.contains<Spawner>(entities[i]) == (i < targets));
    }
    for (size_t i = 0; i < targets; ++i) {
      REQUIRE(world.get<Spawner>(entities[i]).count == 5);
    }
  }

  SECTION("A Loop Adds Or Modifies Components Of Entities That Other Threads Go For As Well") {
    // Every entity hits one of a few that have no Spawner yet: the first hit
    // makes it, every other one counts.
    constexpr size_t targets = 10;
    scheduler.addParallel([&](Edit<Spawner> edit, const Position &pos) {
      const Entity target = entities[static_cast<size_t>(pos.x) % targets];
      edit.emplaceOrModify(target, [](Spawner &spawner) { ++spawner.count; }, 1);
    });
    scheduler.run(world);

    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.contains<Spawner>(entities[i]) == (i < targets));
    }
    for (size_t i = 0; i < targets; ++i) {
      // How many entities hit it.
      REQUIRE(world.get<Spawner>(entities[i]).count ==
              static_cast<int>((entities.size() / targets) + (i < entities.size() % targets ? 1 : 0)));
    }
  }

  SECTION("A System Added With add Gives New Entities Their Components") {
    // emplace without the bit: one thread, and entities nobody else knows.
    scheduler.add([](Query<const Velocity> query, Edit<Frozen, Spawner> edit, Spawn spawn) {
      query.each([&](const Velocity &) {
        const Entity made = spawn.create();
        REQUIRE(edit.emplace<Frozen>(made) == sc::ecs::Emplaced::Made);
        REQUIRE(edit.emplace<Spawner>(made, 4) == sc::ecs::Emplaced::Made);
      });
    });
    scheduler.run(world);
    size_t made = 0;
    world.each<const Frozen, const Spawner>([&](const Spawner &spawner) { made += spawner.count == 4 ? 1 : 0; });
    REQUIRE(made == entities.size() - ((entities.size() + 2) / 3));
  }

  SECTION("A System Added With add Edits Other Entities") {
    // The same as from a parallel loop, on one thread: through the system
    // itself and through a listener of an event it emits.
    constexpr size_t targets = 10;
    scheduler.on([](const Hit &hit, Edit<Spawner> edit) {
      edit.emplaceOrModify(hit.target, [&](Spawner &spawner) { spawner.count += hit.damage; }, hit.damage);
    });
    scheduler.add([&](Query<const Position> query, Edit<Frozen, Target> edit, Emit<Hit> emit) {
      query.each([&](Entity entity, const Position &pos) {
        const Entity target = entities[static_cast<size_t>(pos.x) % targets];
        emit(Hit{.target = target, .damage = 1});
        edit.tryEmplace<Frozen>(target);
        edit.emplaceOrModify(target, [](Target &aim) { ++aim.value; }, 1);
        if (static_cast<size_t>(pos.x) % 2 == 0) {
          edit.modify(entity, [](Target &aim) { aim.value += 100; });
        }
      });
      // Replaced, then erased again for the odd targets.
      for (size_t i = 0; i < targets; ++i) {
        REQUIRE(edit.emplaceOrReplace<Target>(entities[i], -1) == sc::ecs::Emplaced::Replaced);
        if (i % 2 == 1) {
          REQUIRE(edit.erase<Frozen>(entities[i]));
        }
      }
    });
    scheduler.run(world);

    const auto hits = [&](size_t i) {
      return static_cast<int>((entities.size() / targets) + (i < entities.size() % targets ? 1 : 0));
    };
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.contains<Frozen>(entities[i]) == (i < targets && i % 2 == 0));
      REQUIRE(world.contains<Target>(entities[i]) == (i < targets));
      REQUIRE(world.contains<Spawner>(entities[i]) == (i < targets));
    }
    for (size_t i = 0; i < targets; ++i) {
      REQUIRE(world.get<Target>(entities[i]).value == -1);
      REQUIRE(world.get<Spawner>(entities[i]).count == hits(i));
    }
  }

  SECTION("A Loop Edits Its Own Entity Without The Bit") {
    // Each thread changes only the entity of its call: no bit needed. Catch2
    // is for one thread, so the threads count what went wrong.
    std::atomic<int> wrong = 0;
    scheduler.addParallel([&](Entity entity, LocalEdit<Health, Spawner, Frozen, Target> edit, const Position &pos) {
      const auto x = static_cast<int>(pos.x);
      wrong += !edit.modify(entity, [](Health &health) { health.value += 1; });
      wrong += edit.emplaceOrModify(entity, [](Spawner &spawner) { ++spawner.count; }, 1) != sc::ecs::Emplaced::Made;
      wrong +=
          edit.emplaceOrModify(entity, [](Spawner &spawner) { ++spawner.count; }, 1) != sc::ecs::Emplaced::Modified;
      wrong += edit.tryEmplace<Frozen>(entity) != sc::ecs::Emplaced::Made;
      wrong += edit.tryEmplace<Frozen>(entity) != sc::ecs::Emplaced::There;
      wrong += edit.emplaceOrReplace<Target>(entity, x) != sc::ecs::Emplaced::Made;
      wrong += edit.emplaceOrReplace<Target>(entity, -x) != sc::ecs::Emplaced::Replaced;
      if (x % 2 == 0) {
        wrong += !edit.erase<Frozen>(entity);
      }
      // Without a look, and with one.
      wrong += edit.get<Health>(entity).value != 101;
      edit.get<Health>(entity).value += 1;
      wrong += edit.get<const Spawner>(entity).count != 2;
      wrong += edit.tryGet<Target>(entity) != &edit.get<Target>(entity);
      wrong += edit.tryGet<const Health>(entity) != &edit.get<Health>(entity);
    });
    scheduler.run(world);
    REQUIRE(wrong == 0);
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.get<Health>(entities[i]).value == 102);
      REQUIRE(world.get<Spawner>(entities[i]).count == 2);
      REQUIRE(world.get<Target>(entities[i]).value == -static_cast<int>(i));
      REQUIRE(world.contains<Frozen>(entities[i]) == (i % 2 != 0));
    }
  }

  SECTION("A Loop Removes A Component With A Destructor Without The Bit") {
    // Named gives back what it owns: every one that is made is gone again.
    const auto alive = std::make_shared<int>(0);
    scheduler.addParallel([&](Entity entity, LocalEdit<Named> edit, const Position &) {
      edit.emplace<Named>(entity, std::string(40, 'n'), alive);
    });
    Scheduler remover;
    std::atomic<size_t> removed = 0;
    remover.addParallel([&](Entity entity, LocalEdit<Named> edit, const Position &pos) {
      if (static_cast<size_t>(pos.x) % 2 == 0) {
        removed += edit.erase<Named>(entity) ? 1 : 0;
        // Gone already.
        removed += edit.erase<Named>(entity) ? 1 : 0;
      }
    });
    scheduler.run(world);
    REQUIRE(alive.use_count() == 1 + static_cast<long>(entities.size()));
    remover.run(world);
    REQUIRE(removed == (entities.size() + 1) / 2);
    REQUIRE(alive.use_count() == 1 + static_cast<long>(entities.size() / 2));
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.contains<Named>(entities[i]) == (i % 2 != 0));
    }
  }

  SECTION("A Loop Modifies Entities That Other Threads Go For As Well") {
    // Every entity hits one of a few, as do many on other threads, and
    // every hit counts.
    constexpr size_t targets = 10;
    std::atomic<size_t> missed = 0;
    scheduler.addParallel([&](Edit<Health, Spawner> edit, const Position &pos) {
      const Entity target = entities[static_cast<size_t>(pos.x) % targets];
      edit.modify(target, [](Health &health) { health.value -= 1; });
      // No entity has one.
      missed += !edit.modify(target, [](Spawner &spawner) { ++spawner.count; });
    });
    scheduler.run(world);
    scheduler.run(world);

    REQUIRE(missed == 2 * entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
      // How many entities hit it, twice each.
      const int hits = i < targets ? static_cast<int>((entities.size() - i + targets - 1) / targets) : 0;
      REQUIRE(world.get<Health>(entities[i]).value == 100 - (2 * hits));
    }
  }

  SECTION("A Component Is Removed Once The Stage Is Over") {
    constexpr size_t targets = 10;
    for (size_t i = 0; i < targets; ++i) {
      world.emplace<Spawner>(entities[i], 7);
    }
    // Every entity uses the component of one of a few and wants it gone, as
    // do many on other threads.
    std::atomic<size_t> used = 0;
    scheduler.addParallel([&](Commands commands, Query<const Spawner> query, const Position &pos) {
      const Entity target = entities[static_cast<size_t>(pos.x) % targets];
      used += query.contains<Spawner>(target) && query.get<const Spawner>(target).count == 7;
      commands.erase<Spawner>(target);
      // One it does not have, and one of an entity that will be gone.
      commands.erase<Frozen>(target);
      if (static_cast<size_t>(pos.x) >= entities.size() - 5) {
        commands.erase<Health>(entities[entities.size() - 1]);
        commands.destroy(entities[entities.size() - 1]);
      }
    });
    // Next to it, although it reads the component: it is still there.
    std::atomic<size_t> seen = 0;
    scheduler.addParallel([&](const Spawner &spawner) { seen += spawner.count == 7; });
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
    scheduler.run(world);

    REQUIRE(used == entities.size());
    REQUIRE(seen == targets);
    REQUIRE(world.column<Spawner>().pages() == world.column<Spawner>().spares());
    REQUIRE_FALSE(world.valid(entities.back()));
    for (size_t i = 0; i + 1 < entities.size(); ++i) {
      REQUIRE(world.contains<Health>(entities[i]));
    }

    // Nothing is left over for the next run, and what comes back stays.
    world.emplace<Spawner>(entities[0], 7);
    scheduler.run(world);
    REQUIRE(seen == targets + 1);
    REQUIRE_FALSE(world.contains<Spawner>(entities[0]));
  }

  SECTION("A Component Is Added Once The Stage Is Over") {
    constexpr size_t targets = 10;
    std::atomic<size_t> seen = 0;
    scheduler.addParallel([&](Entity entity, Commands commands, const Position &pos) {
      commands.emplace<Spawner>(entity, static_cast<int>(pos.x));
      commands.emplace<Frozen>(entity);
      // One that many on other threads want to give as well.
      commands.emplace<Target>(entities[static_cast<size_t>(pos.x) % targets], 7);
    });
    // Next to it, so no entity has any of them yet.
    scheduler.addParallel<Query<const Position, Exclude<Spawner, Frozen, Target>>>([&](const Position &) { ++seen; });
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
    scheduler.run(world);

    REQUIRE(seen == entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.get<Spawner>(entities[i]).count == static_cast<int>(i));
      REQUIRE(world.contains<Frozen>(entities[i]));
      REQUIRE(world.contains<Target>(entities[i]) == (i < targets));
    }
    REQUIRE(world.get<Target>(entities[0]).value == 7);

    // An entity that has the component keeps the one it has.
    world.get<Spawner>(entities[0]).count = -1;
    seen = 0;
    scheduler.run(world);
    REQUIRE(seen == 0);
    REQUIRE(world.get<Spawner>(entities[0]).count == -1);
  }

  SECTION("Threads Outside The Pool Record Through Commands") {
    constexpr size_t threadCount = 8;
    scheduler.add([&](Commands commands) {
      std::vector<std::jthread> threads;
      for (size_t t = 0; t < threadCount; ++t) {
        threads.emplace_back([&, t] {
          for (size_t i = t; i < entities.size(); i += threadCount) {
            commands.emplace<Spawner>(entities[i], static_cast<int>(i));
            if (i % 5 == 0) {
              commands.destroy(entities[i]);
            }
          }
        });
      }
    });
    scheduler.run(world);

    for (size_t i = 0; i < entities.size(); ++i) {
      if (i % 5 == 0) {
        REQUIRE_FALSE(world.valid(entities[i]));
      } else {
        REQUIRE(world.get<Spawner>(entities[i]).count == static_cast<int>(i));
      }
    }
  }

  SECTION("Recorded Components Are Added And Removed In Their Order") {
    world.emplace<Spawner>(entities[0], 1);
    world.emplace<Spawner>(entities[1], 1);
    const Entity gone = entities[2];
    Entity made = sc::ecs::null;
    scheduler.add([&](Commands commands) {
      // Replaced, gone again, and of an entity that is there right away.
      commands.erase<Spawner>(entities[0]);
      commands.emplace<Spawner>(entities[0], 2);
      commands.emplace<Target>(entities[1], 3);
      commands.erase<Target>(entities[1]);
      made = commands.create();
      commands.emplace<Spawner>(made, 4);
      // Nothing for an entity that is no more, or about to be.
      commands.emplace<Spawner>(gone, 5);
      commands.destroy(entities[3]);
      commands.emplace<Spawner>(entities[3], 6);
    });
    world.destroy(gone);
    scheduler.run(world);

    REQUIRE(world.get<Spawner>(entities[0]).count == 2);
    REQUIRE_FALSE(world.contains<Target>(entities[1]));
    REQUIRE(world.valid(made));
    REQUIRE(world.get<Spawner>(made).count == 4);
    REQUIRE_FALSE(world.valid(entities[3]));
    // Not left for whoever gets the index next.
    const Entity next = world.create();
    const Entity other = world.create();
    REQUIRE_FALSE(world.contains<Spawner>(next));
    REQUIRE_FALSE(world.contains<Spawner>(other));
  }

  SECTION("A Recorded Component That Is Not Given Is Gone") {
    const auto alive = std::make_shared<int>(0);
    // Too long to be kept in the string itself.
    const std::string name(100, 'n');
    bool fails = false;
    scheduler.addParallel([&](Entity entity, Commands commands, const Position &) {
      commands.emplace<Named>(entity, name, alive);
      // The entity has one by then.
      commands.emplace<Named>(entity, name, alive);
    });
    scheduler.add([&](Query<const Health>) {
      if (fails) {
        throw std::runtime_error("failed");
      }
    });
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
    scheduler.run(world);
    REQUIRE(alive.use_count() == 1 + static_cast<long>(entities.size()));
    for (const Entity entity: entities) {
      REQUIRE(world.get<Named>(entity).name == name);
    }

    // Many more than fit into the memory kept for them, none of them given.
    for (int run = 0; run < 3; ++run) {
      scheduler.run(world);
      REQUIRE(alive.use_count() == 1 + static_cast<long>(entities.size()));
    }

    // Nor are the ones of a stage that failed.
    for (const Entity entity: entities) {
      world.erase<Named>(entity);
    }
    REQUIRE(alive.use_count() == 1);
    fails = true;
    REQUIRE_THROWS_AS(scheduler.run(world), std::runtime_error);
    REQUIRE(alive.use_count() == 1);
    REQUIRE_FALSE(world.contains<Named>(entities[0]));
  }

  SECTION("A Recorded Component That Cannot Be Made Ends The Run") {
    const auto alive = std::make_shared<int>(0);
    int ran = 0;
    scheduler.add([&](Commands commands) {
      commands.emplace<Spawner>(entities[0], 1);
      commands.emplace<Fragile>(entities[0], 2);
      commands.emplace<Named>(entities[0], "after", alive);
      commands.destroy(entities[1]);
    });
    scheduler.add([&](World &) { ++ran; });

    Fragile::breaks = true;
    REQUIRE_THROWS_AS(scheduler.run(world), std::runtime_error);
    Fragile::breaks = false;
    // What came before it is done, the rest is dropped.
    REQUIRE(ran == 0);
    REQUIRE(world.get<Spawner>(entities[0]).count == 1);
    REQUIRE_FALSE(world.contains<Fragile>(entities[0]));
    REQUIRE_FALSE(world.contains<Named>(entities[0]));
    REQUIRE(alive.use_count() == 1);
    REQUIRE(world.valid(entities[1]));

    // And nothing of it is left over for the next run.
    world.erase<Spawner>(entities[0]);
    scheduler.run(world);
    REQUIRE(ran == 1);
    REQUIRE(world.get<Fragile>(entities[0]).value == 2);
    REQUIRE(alive.use_count() == 2);
    REQUIRE_FALSE(world.valid(entities[1]));
  }

  SECTION("Recorded Changes Of Several Components Over Several Pages Leave The Counts Right") {
    // Every page of every column is done on one thread, its adds counted
    // ahead and the ones not done given back: the count of every page must
    // be what its mask says.
    const Entity gone = world.create();
    world.destroy(gone);
    scheduler.addParallel([gone](Entity entity, Commands commands, const Position &pos) {
      const auto x = static_cast<int>(pos.x);
      commands.emplace<Spawner>(entity, x);
      commands.emplace<Target>(entity, x);
      // Erased and made again: the later one stays.
      commands.erase<Spawner>(entity);
      commands.emplace<Spawner>(entity, -x);
      // Has one already: not done, its count given back.
      commands.emplace<Health>(entity);
      // Of an entity that is gone: dropped.
      commands.emplace<Target>(gone, 1);
      if (x % 3 == 0) {
        commands.erase<Target>(entity);
      }
    });
    scheduler.run(world);
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.get<Spawner>(entities[i]).count == -static_cast<int>(i));
      REQUIRE(world.contains<Target>(entities[i]) == (i % 3 != 0));
      REQUIRE(world.get<Health>(entities[i]).value == 100);
    }
    const auto countsRight = [&](const auto &column) {
      for (uint32_t number = 0; number < world.pageCount(); ++number) {
        const auto page = column.page(number);
        uint32_t bits = 0;
        if (page) {
          for (size_t word = 0; word < sc::ecs::internal::PAGE_WORDS; ++word) {
            const uint64_t mask = page.mask[word].load();
            bits += static_cast<uint32_t>(std::popcount(mask));
            // Loops and erases look at the words of the summary only.
            if (mask != 0 && (page.summary & (uint64_t{1} << word)) == 0) {
              return false;
            }
          }
        }
        if (bits != page.count) {
          return false;
        }
      }
      return true;
    };
    REQUIRE(countsRight(world.column<Spawner>()));
    REQUIRE(countsRight(world.column<Target>()));
    REQUIRE(countsRight(world.column<Health>()));
    // A full page adds Spawner twice per entity, more than counted ahead at
    // first: had the count of the page reached zero with the last erase, the
    // summary would have lost the words of the components made again.
  }

  SECTION("Recorded Changes Of Values Are Done In Their Order") {
    const auto alive = std::make_shared<int>(0);
    world.emplace<Spawner>(entities[0], 1);
    const Entity gone = entities[2];
    world.emplace<Spawner>(gone, 1);
    Entity made = sc::ecs::null;
    int called = 0;
    scheduler.add([&](Commands commands) {
      // In the order recorded with the other changes of the component.
      commands.modify(entities[0], [](Spawner &spawner) { spawner.count *= 10; });
      commands.erase<Spawner>(entities[0]);
      commands.emplace<Spawner>(entities[0], 2);
      commands.modify(entities[0], [](Spawner &spawner) { spawner.count += 1; });
      made = commands.create();
      commands.emplace<Spawner>(made, 4);
      commands.modify(made, [](Spawner &spawner) { spawner.count += 1; });
      // Nothing for an entity without the component, or one that is no more,
      // or none at all.
      commands.modify(entities[1], [&](Spawner &) { ++called; });
      commands.modify(gone, [&](Spawner &) { ++called; });
      commands.modify(sc::ecs::null, [&, alive](Spawner &) { ++called; });
      commands.emplace<Named>(sc::ecs::null, "nobody", alive);
    });
    world.destroy(gone);
    scheduler.run(world);

    REQUIRE(world.get<Spawner>(entities[0]).count == 3);
    REQUIRE(world.get<Spawner>(made).count == 5);
    REQUIRE(called == 0);
    REQUIRE(alive.use_count() == 1);
  }

  SECTION("Recorded Components Replace The Ones The Entities Have") {
    // Over several pages, on several threads: every other entity has the
    // component already.
    for (size_t i = 0; i < entities.size(); i += 2) {
      world.emplace<Spawner>(entities[i], -1);
    }
    const auto alive = std::make_shared<int>(0);
    world.emplace<Named>(entities[0], "old", alive);
    const Entity gone = entities[1];
    scheduler.addParallel([&](Entity entity, Commands commands, const Position &pos) {
      commands.emplaceOrReplace<Spawner>(entity, static_cast<int>(pos.x));
    });
    scheduler.add([&](Commands commands) {
      // In their order with the other changes: the last one stays.
      commands.emplaceOrReplace<Named>(entities[0], "first", alive);
      commands.modify(entities[0], [](Named &named) { named.name += " changed"; });
      commands.emplaceOrReplace<Named>(entities[0], "last", alive);
      commands.emplaceOrReplace<Named>(entities[2], "made", alive);
      // Dropped: an entity that is no more.
      commands.emplaceOrReplace<Named>(gone, "nobody", alive);
    });
    world.destroy(gone);
    scheduler.run(world);

    size_t spawners = 0;
    for (size_t i = 0; i < entities.size(); ++i) {
      if (i != 1) {
        REQUIRE(world.get<const Spawner>(entities[i]).count == static_cast<int>(i));
        ++spawners;
      }
    }
    size_t looped = 0;
    world.each<const Spawner>([&](const Spawner &) { ++looped; });
    REQUIRE(looped == spawners);
    REQUIRE(world.get<const Named>(entities[0]).name == "last");
    REQUIRE(world.get<const Named>(entities[2]).name == "made");
    // The two that stay; every other one that was made is gone.
    REQUIRE(alive.use_count() == 3);
  }

  SECTION("Recorded Changes Of Many Entities Keep Their Order Per Entity") {
    // Done on several threads, a page each, with the erases of a page
    // gathered: whatever comes after an erase of the same entity must see it
    // done, whatever comes before must not.
    const auto alive = std::make_shared<int>(0);
    scheduler.addParallel([&](Entity entity, Commands commands, const Position &pos) {
      const auto i = static_cast<size_t>(pos.x);
      if (i % 97 == 0) {
        // Nobody's: only dropped, also what the function holds.
        commands.emplace<Named>(sc::ecs::null, "nobody", alive);
        commands.modify(sc::ecs::null, [alive](Health &health) { health.value = -2; });
      }
      switch (i % 5) {
        case 0:
          commands.erase<Health>(entity);
          commands.emplace<Health>(entity, 7);
          commands.modify(entity, [](Health &health) { health.value += 1; });
          break;
        case 1:
          commands.erase<Health>(entity);
          break;
        case 2:
          // Has one: dropped, and the modify sees the old one.
          commands.emplace<Health>(entity, 5);
          commands.modify(entity, [](Health &health) { health.value -= 1; });
          break;
        case 3:
          commands.modify(entity, [](Health &health) { health.value *= 2; });
          commands.erase<Health>(entity);
          commands.emplace<Named>(entity, "named", alive);
          commands.erase<Named>(entity);
          break;
        default:
          commands.emplace<Named>(entity, "kept", alive);
          commands.destroy(entity);
          // Still done: entities go after all changes.
          commands.modify(entity, [](Health &health) { health.value = -1; });
          break;
      }
    });
    scheduler.run(world);

    size_t healthy = 0;
    for (size_t i = 0; i < entities.size(); ++i) {
      const Entity entity = entities[i];
      switch (i % 5) {
        case 0:
          REQUIRE(world.get<Health>(entity).value == 8);
          ++healthy;
          break;
        case 1:
        case 3:
          REQUIRE_FALSE(world.contains<Health>(entity));
          REQUIRE_FALSE(world.contains<Named>(entity));
          break;
        case 2:
          REQUIRE(world.get<Health>(entity).value == 99);
          ++healthy;
          break;
        default:
          REQUIRE_FALSE(world.valid(entity));
          break;
      }
    }
    // The counts of the pages agree with what is there, and what was owned
    // is given back.
    size_t counted = 0;
    world.each<const Health>([&](const Health &) { ++counted; });
    REQUIRE(counted == healthy);
    REQUIRE(world.column<Health>().page(0).count + world.column<Health>().page(1).count +
                world.column<Health>().page(2).count ==
            healthy);
    REQUIRE(alive.use_count() == 1);
    REQUIRE(world.column<Named>().pages() == world.column<Named>().spares());
  }

  SECTION("Threads Record Changes For The Same Components, Done Once The Stage Is Over") {
    // Every entity hits one of a few, as do many on other threads, and
    // every hit counts.
    constexpr size_t targets = 10;
    scheduler.addParallel([&](Commands commands, const Position &pos) {
      commands.modify(entities[static_cast<size_t>(pos.x) % targets], [](Health &health) { health.value -= 1; });
    });
    // Runs at the same time, as recording does not count as writing, and
    // sees the values as they were.
    std::atomic<size_t> changed = 0;
    scheduler.addParallel([&](const Health &health) { changed += health.value != 100; });
    REQUIRE(scheduler.stages() == Stages{{0, 1}});
    scheduler.run(world);
    REQUIRE(changed == 0);
    scheduler.run(world);
    REQUIRE(changed == targets);

    for (size_t i = 0; i < entities.size(); ++i) {
      // How many entities hit it, twice each.
      const int hits = i < targets ? static_cast<int>((entities.size() - i + targets - 1) / targets) : 0;
      REQUIRE(world.get<Health>(entities[i]).value == 100 - (2 * hits));
    }
  }

  SECTION("A Recorded Change That Throws Ends The Run") {
    const auto alive = std::make_shared<int>(0);
    int ran = 0;
    bool throws = true;
    scheduler.add([&](Commands commands) {
      commands.modify(entities[0], [](Health &health) { health.value = 1; });
      commands.modify(entities[0], [&throws](Health &health) {
        health.value += 1;
        if (throws) {
          throw std::runtime_error("modify");
        }
      });
      commands.modify(entities[0], [alive](Health &health) { health.value += 10; });
      commands.destroy(entities[1]);
    });
    scheduler.add([&](World &) { ++ran; });

    REQUIRE_THROWS_AS(scheduler.run(world), std::runtime_error);
    // What came before it is done, also what it did itself, the rest is
    // dropped along with what it holds.
    REQUIRE(ran == 0);
    REQUIRE(world.get<Health>(entities[0]).value == 2);
    REQUIRE(alive.use_count() == 1);
    REQUIRE(world.valid(entities[1]));

    // And nothing of it is left over for the next run.
    throws = false;
    scheduler.run(world);
    REQUIRE(ran == 1);
    REQUIRE(world.get<Health>(entities[0]).value == 12);
    REQUIRE(alive.use_count() == 1);
    REQUIRE_FALSE(world.valid(entities[1]));
  }

  SECTION("A System That Takes The World May Do Anything") {
    scheduler.add([&](World &all) {
      for (size_t i = 0; i < entities.size(); i += 2) {
        all.destroy(entities[i]);
      }
    });
    scheduler.addParallel([](Health &health) { health.value += 1; });
    scheduler.run(world);
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.valid(entities[i]) == (i % 2 != 0));
      if (i % 2 != 0) {
        REQUIRE(world.get<Health>(entities[i]).value == 101);
      }
    }
  }

  SECTION("Systems Load Into The Provided Cache") {
    ResourceCache<Texture> textures;
    std::atomic<int> loads = 0;
    scheduler.provide(textures);
    scheduler.addParallel([&](Entity entity, Commands commands, Resources<Texture> cache, const Velocity &) {
      // Four keys for all entities: each is loaded once.
      const int id = static_cast<int>(sc::ecs::indexOf(entity) % 4);
      ResourceHandle<Texture> albedo = cache->load(static_cast<sc::h64>(id), [&] {
        ++loads;
        return Texture{id};
      });
      commands.emplace<Material>(entity, std::move(albedo));
    });
    scheduler.run(world);
    // CHECK up to the erase: the cache must not go while components have its handles.
    CHECK(loads == 4);
    CHECK(textures.size() == 4);
    for (size_t i = 0; i < entities.size(); ++i) {
      if (i % 3 != 0) {
        CHECK(world.get<Material>(entities[i]).albedo->id == static_cast<int>(sc::ecs::indexOf(entities[i]) % 4));
      }
    }

    // Without handles, a system gives them back.
    for (const Entity entity: entities) {
      world.erase<Material>(entity);
    }
    Scheduler collector;
    size_t collected = 0;
    collector.provide(textures);
    collector.add([&](const Resources<Texture> &cache) { collected = cache->collect(); }, "collect textures");
    collector.run(world);
    REQUIRE(collected == 4);
    REQUIRE(textures.size() == 0);
  }

  SECTION("Systems Write And Read A Global Of The World") {
    world.setGlobal<GameTime>(0.0f);
    scheduler.add([](Global<GameTime> time) { time->seconds += 1.0f; });
    // Every thread of the loop sees what the stage before wrote.
    std::atomic<size_t> seen = 0;
    std::atomic<size_t> late = 0;
    scheduler.addParallel([&](Global<const GameTime> time, const Position &) {
      seen.fetch_add(1, std::memory_order_relaxed);
      if (time->seconds != 1.0f) {
        late.fetch_add(1, std::memory_order_relaxed);
      }
    });
    REQUIRE(scheduler.stages() == Stages{{0}, {1}});
    scheduler.run(world);
    REQUIRE(seen == entities.size());
    REQUIRE(late == 0);
    REQUIRE(world.global<GameTime>().seconds == 1.0f);
  }

  SECTION("Hooks See Every Component Added Right Away") {
    std::atomic<int> calls = 0;
    world.onAdd<Spawner>([&](Entity, Spawner &spawner) {
      calls.fetch_add(1, std::memory_order_relaxed);
      spawner.count += 100;
    });
    const Entity entity = world.create();
    world.emplace<Spawner>(entity, 1);
    REQUIRE(world.get<const Spawner>(entity).count == 101);
    world.erase<Spawner>(entity);
    REQUIRE(world.tryEmplace<Spawner>(entity, 2) == sc::ecs::Emplaced::Made);
    REQUIRE(world.tryEmplace<Spawner>(entity, 3) == sc::ecs::Emplaced::There);
    // Replaced, not added: no call.
    REQUIRE(world.emplaceOrReplace<Spawner>(entity, 4) == sc::ecs::Emplaced::Replaced);
    REQUIRE(world.get<const Spawner>(entity).count == 4);
    world.erase<Spawner>(entity);
    world.emplaceOrModify<Spawner>(entity, [](Spawner &) {}, 5);
    REQUIRE(world.get<const Spawner>(entity).count == 105);
    const Entity made = world.create(Spawner{6}, Target{});
    REQUIRE(world.get<const Spawner>(made).count == 106);
    REQUIRE(calls == 4);
    std::vector<Entity> many(PAGE_SIZE + 10);
    world.create(many, Spawner{7});
    REQUIRE(calls == 4 + static_cast<int>(many.size()));
    REQUIRE(world.get<const Spawner>(many.back()).count == 107);
  }

  SECTION("Hooks Of Adds In A Loop Get The Parameters Of Its System") {
    std::atomic<size_t> calls = 0;
    world.onAdd<Spawner>([&](Entity entity, Spawner &, Commands commands) {
      calls.fetch_add(1, std::memory_order_relaxed);
      commands.emplace<Target>(entity, 7);
    });
    scheduler.addParallel(
        [](Entity entity, Edit<Spawner> edit, const Position &) { edit.tryEmplace<Spawner>(entity); });
    scheduler.addParallel(
        [](Entity entity, LocalEdit<Frozen> edit, const Position &) { edit.emplace<Frozen>(entity); });
    scheduler.run(world);
    REQUIRE(calls == entities.size());
    for (const Entity entity: entities) {
      REQUIRE(world.get<const Target>(entity).value == 7);
    }
  }

  SECTION("Hooks Of Adds Through Commands Run After Them, In Rounds") {
    std::atomic<size_t> targets = 0;
    world.onAdd<Spawner>([](Entity entity, Spawner &, Commands commands) { commands.emplace<Target>(entity, 8); });
    world.onAdd<Target>([&](Entity, Target &target) {
      targets.fetch_add(1, std::memory_order_relaxed);
      target.value += 1;
    });
    scheduler.addParallel(
        [](Entity entity, Commands commands, const Position &) { commands.emplace<Spawner>(entity, 1); });
    scheduler.run(world);
    REQUIRE(targets == entities.size());
    for (const Entity entity: entities) {
      REQUIRE(world.get<const Target>(entity).value == 9);
    }
  }

  SECTION("Hooks That Never Stop Adding End The Run") {
    world.onAdd<Spawner>([](Entity entity, Spawner &, Commands commands) {
      commands.erase<Spawner>(entity);
      commands.emplace<Spawner>(entity);
    });
    scheduler.add([&](Commands commands) { commands.emplace<Spawner>(entities[0]); });
    REQUIRE_THROWS_AS(scheduler.run(world), std::logic_error);
  }

  SECTION("A Hook With Commands Outside A System Throws") {
    world.onAdd<Frozen>([](Entity, Frozen &, Commands) {});
    REQUIRE_THROWS_AS(world.emplace<Frozen>(entities[0]), std::logic_error);
  }

  SECTION("A Global The World Does Not Have Ends The Run") {
    bool ran = false;
    scheduler.add([&](Global<const GameTime>) { ran = true; }, "tick");
    REQUIRE_THROWS_AS(scheduler.run(world), std::logic_error);
    REQUIRE_FALSE(ran);
  }

  SECTION("A Cache That Was Not Provided Ends The Run") {
    bool ran = false;
    scheduler.add([&](Resources<Texture>) { ran = true; }, "stream");
    REQUIRE_THROWS_AS(scheduler.run(world), std::logic_error);
    REQUIRE_FALSE(ran);
  }

  SECTION("One Cache Per Type") {
    ResourceCache<Texture> textures;
    ResourceCache<Texture> others;
    ResourceCache<Texture, std::string> named;
    scheduler.provide(textures);
    REQUIRE_THROWS_AS(scheduler.provide(others), std::logic_error);
    // Another key is another type.
    scheduler.provide(named);
    Texture *got = nullptr;
    Texture *gotNamed = nullptr;
    scheduler.add([&](Resources<Texture> cache, Resources<Texture, std::string> byName) {
      got = &*cache->load(1, [] { return Texture{1}; });
      gotNamed = &*byName->load("one", [] { return Texture{2}; });
    });
    scheduler.run(world);
    REQUIRE(got->id == 1);
    REQUIRE(gotNamed->id == 2);
    textures.collect();
    named.collect();
  }

  SECTION("Listeners Run Where The Event Is Emitted") {
    const Entity boss = entities[0];
    size_t moving = 0;
    for (size_t i = 0; i < entities.size(); ++i) {
      moving += (i % 3 != 0) ? 1 : 0;
    }
    // Many threads hit the same target at once.
    scheduler.addParallel([boss](Emit<Hit> emit, const Velocity &) { emit(Hit{.target = boss, .damage = 1}); });
    scheduler.on([](const Hit &hit, Edit<Health> edit) {
      edit.modify(hit.target, [&](Health &health) { health.value -= hit.damage; });
    });
    scheduler.run(world);
    REQUIRE(world.get<Health>(boss).value == 100 - static_cast<int>(moving));
  }

  SECTION("Listeners Emit Events And Record Through Commands") {
    // The system itself takes no Commands: its lists come from the listener.
    scheduler.addParallel([](Entity entity, Emit<Hit> emit, const Velocity &) { emit(Hit{.target = entity}); });
    scheduler.on([](const Hit &hit, Emit<Died> died) { died(Died{.entity = hit.target}); });
    scheduler.on([](const Died &died, Commands commands) { commands.destroy(died.entity); });
    scheduler.run(world);
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.valid(entities[i]) == (i % 3 == 0));
    }
  }

  SECTION("An Event Without Listeners Does Nothing") {
    std::atomic<int> ran = 0;
    scheduler.addParallel([&](Emit<Died> emit, const Velocity &) {
      emit(Died{});
      ++ran;
    });
    scheduler.run(world);
    REQUIRE(ran > 0);
  }

  SECTION("The Columns Of All Systems Are Made Before The First") {
    World fresh;
    std::atomic<bool> there = false;
    scheduler.add([&](Query<const Spawner, Exclude<Frozen>>, Emit<Hit>) {
      // Made already, also the one of the listener of Hit.
      there = fresh.findColumn<Spawner>() != nullptr && fresh.findColumn<Target>() != nullptr;
    });
    scheduler.on([](const Hit &, Edit<Target>) {});
    REQUIRE(fresh.findColumn<Spawner>() == nullptr);
    scheduler.run(fresh);
    REQUIRE(there);
    // An excluded component is only looked up.
    REQUIRE(fresh.findColumn<Frozen>() == nullptr);
    // And the next run finds them.
    scheduler.run(fresh);
    REQUIRE(there);
  }

  SECTION("An Exception Ends The Run After Its Stage") {
    std::atomic<int> ran = 0;
    scheduler.addParallel([](Entity entity, Commands commands, const Position &) { commands.destroy(entity); });
    scheduler.add([&](Query<const Health>) {
      ++ran;
      throw std::runtime_error("failed");
    });
    scheduler.add([&](Query<const Velocity>) { ++ran; });
    scheduler.add([&](Query<Health>) { ++ran; });
    REQUIRE(scheduler.stages() == Stages{{0, 1, 2}, {3}});
    REQUIRE_THROWS_AS(scheduler.run(world), std::runtime_error);
    // The systems next to it finished, the later one did not start, and what
    // was recorded is dropped.
    REQUIRE(ran == 2);
    for (const Entity entity: entities) {
      REQUIRE(world.valid(entity));
    }
  }
}
