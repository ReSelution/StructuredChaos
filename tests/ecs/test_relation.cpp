#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

#include "sc/ecs/relation.hpp"
#include "sc/ecs/scheduler.hpp"
#include "sc/ecs/world.hpp"
#include "sc/threading/threading.hpp"

namespace {

  using sc::ecs::Cascade;
  using sc::ecs::ChildOf;
  using sc::ecs::Children;
  using sc::ecs::Commands;
  using sc::ecs::Edit;
  using sc::ecs::Entity;
  using sc::ecs::Parent;
  using sc::ecs::Query;
  using sc::ecs::Related;
  using sc::ecs::Relation;
  using sc::ecs::Scheduler;
  using sc::ecs::World;

  struct Position {
    float x{0.0f};
  };

  // A relation whose sources stay when their target goes.
  struct OwnedBy {};

  [[nodiscard]] std::vector<Entity> childrenOf(const World &world, Entity parent) {
    std::vector<Entity> found;
    sc::ecs::forEachRelated<ChildOf>(world, parent, [&](Entity child) { found.push_back(child); });
    return found;
  }

} // namespace

TEST_CASE("ECS Relations", "[ecs][relation][threading]") {
  sc::threading::init();

  World world;
  sc::ecs::relate<ChildOf>(world, Cascade::Destroy);
  sc::ecs::relate<OwnedBy>(world, Cascade::Orphan);

  SECTION("A Kind Is Related Once") {
    REQUIRE_THROWS_AS(sc::ecs::relate<ChildOf>(world, Cascade::Orphan), std::logic_error);
  }

  SECTION("Outside A System The Lists Follow Right Away, And Children Go With Their Parent") {
    const Entity root = world.create();
    const Entity a = world.create();
    const Entity b = world.create();
    const Entity grandchild = world.create();
    world.emplace<Parent>(a, root);
    world.emplace<Parent>(b, root);
    world.emplace<Parent>(grandchild, a);
    REQUIRE(childrenOf(world, root) == std::vector<Entity>{a, b});
    REQUIRE(childrenOf(world, a) == std::vector<Entity>{grandchild});

    // Taken out of the list when the relation goes, and given another
    // parent by erase and emplace.
    world.erase<Parent>(b);
    REQUIRE(childrenOf(world, root) == std::vector<Entity>{a});
    world.emplace<Parent>(b, a);
    REQUIRE(childrenOf(world, a) == std::vector<Entity>{grandchild, b});

    world.destroy(root);
    REQUIRE_FALSE(world.valid(a));
    REQUIRE_FALSE(world.valid(b));
    REQUIRE_FALSE(world.valid(grandchild));
  }

  SECTION("A Child That Goes Leaves Its Parent's List") {
    const Entity root = world.create();
    const Entity a = world.create();
    const Entity b = world.create();
    world.emplace<Parent>(a, root);
    world.emplace<Parent>(b, root);
    world.destroy(a);
    REQUIRE(childrenOf(world, root) == std::vector<Entity>{b});
    const std::vector<Entity> both{root, b};
    world.destroy(std::span<const Entity>{both});
    REQUIRE_FALSE(world.valid(b));
  }

  SECTION("Orphans Stay Without The Relation") {
    const Entity owner = world.create();
    const Entity sword = world.create();
    world.emplace<Relation<OwnedBy>>(sword, owner);
    world.destroy(owner);
    REQUIRE(world.valid(sword));
    REQUIRE_FALSE(world.contains<Relation<OwnedBy>>(sword));
  }

  SECTION("Descendants Come After Their Parent, Depth First") {
    const Entity root = world.create();
    const Entity a = world.create();
    const Entity b = world.create();
    const Entity a1 = world.create();
    const Entity a2 = world.create();
    const Entity b1 = world.create();
    world.emplace<Parent>(a, root);
    world.emplace<Parent>(b, root);
    world.emplace<Parent>(a1, a);
    world.emplace<Parent>(a2, a);
    world.emplace<Parent>(b1, b);
    std::vector<Entity> order;
    sc::ecs::forEachDescendant<ChildOf>(world, root, [&](Entity entity) { order.push_back(entity); });
    REQUIRE(order == std::vector<Entity>{a, a1, a2, b, b1});
  }

  SECTION("Through Commands On All Threads, In Rounds") {
    // Many children of one root, given on all threads at once, then a tree
    // below every one of them, and all of it gone with the root.
    constexpr size_t COUNT = 3 * size_t{sc::ecs::PAGE_SIZE};
    std::vector<Entity> entities(COUNT);
    for (Entity &entity: entities) {
      entity = world.create(Position{});
    }
    const Entity root = world.create();
    Scheduler scheduler;
    scheduler.addParallel([&](Entity entity, Commands commands, const Position &) {
      const size_t i = sc::ecs::indexOf(entity) - sc::ecs::indexOf(entities.front());
      // Every third a child of the root, the others of the one before.
      commands.emplace<Parent>(entity, i % 3 == 0 ? root : entities[i - 1]);
    });
    scheduler.run(world);
    REQUIRE(childrenOf(world, root).size() == COUNT / 3);
    size_t descendants = 0;
    sc::ecs::forEachDescendant<ChildOf>(world, root, [&](Entity) { ++descendants; });
    REQUIRE(descendants == COUNT);

    Scheduler reaper;
    reaper.add([&](Commands commands) { commands.destroy(root); });
    reaper.run(world);
    REQUIRE(std::ranges::none_of(entities, [&](Entity entity) { return world.valid(entity); }));
  }

  SECTION("Through Edit In A Loop, Read Through A Query") {
    const Entity root = world.create();
    std::vector<Entity> entities(sc::ecs::PAGE_SIZE + 10);
    for (Entity &entity: entities) {
      entity = world.create(Position{});
    }
    Scheduler scheduler;
    scheduler.addParallel(
        [&](Entity entity, Edit<Parent> edit, const Position &) { edit.emplace<Parent>(entity, root); });
    size_t counted = 0;
    scheduler.add([&](Query<const Children> children) {
      sc::ecs::forEachRelated<ChildOf>(children, root, [&](Entity) { ++counted; });
    });
    // The hooks of Parent write Children: the reader comes after.
    REQUIRE(scheduler.stages(world) == std::vector<std::vector<size_t>>{{0}, {1}});
    scheduler.run(world);
    REQUIRE(counted == entities.size());
  }
}
