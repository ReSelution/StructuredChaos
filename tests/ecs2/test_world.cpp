#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sc/ecs2/world.hpp"
#include "sc/threading/threading.hpp"

namespace {

  using sc::ecs2::Entity;
  using sc::ecs2::Exclude;
  using sc::ecs2::PAGE_SIZE;
  using sc::ecs2::World;

  struct Position {
    float x{0.0f};
    float y{0.0f};
  };

  struct Velocity {
    float dx{0.0f};
    float dy{0.0f};
  };

  // Every component carries its entity, so that one that is handed out for
  // another entity shows.
  struct Alpha {
    Entity owner;
  };
  struct Beta {
    Entity owner;
  };
  struct Gamma {
    Entity owner;
    uint64_t more[3]{};
  };
  struct Tag {};

  // Few entities have one.
  struct Player {
    int number{0};
  };

  // What the world should hold: for every entity that exists, a bit for each
  // of the four components above.
  using Model = std::unordered_map<uint64_t, unsigned>;
  constexpr unsigned ALPHA = 1, BETA = 2, GAMMA = 4, TAG = 8;

  // Goes through the entities that have the wanted components and none of the
  // unwanted ones, on one thread and on all, and requires that the function is
  // called once for exactly those entities of the model, with their own
  // components.
  // Room for the rare entities of a test.
  constexpr size_t RARE_PLACES = 2 * PAGE_SIZE;

  template <typename... Terms> void requireLoop(World &world, const Model &model, unsigned wanted, unsigned unwanted) {
    std::vector<uint64_t> expected;
    for (const auto &[entity, components] : model) {
      if ((components & wanted) == wanted && (components & unwanted) == 0) {
        expected.push_back(entity);
      }
    }
    std::ranges::sort(expected);

    std::vector<uint64_t> seen;
    size_t wrong = 0;
    world.each<Terms...>([&](Entity entity, const auto &...components) {
      seen.push_back(static_cast<uint64_t>(entity));
      wrong += ((components.owner != entity) || ... || false);
    });
    std::ranges::sort(seen);
    REQUIRE(wrong == 0);
    REQUIRE(seen == expected);

    // The rare entities, which have the highest indices, follow the others.
    const size_t common = size_t{world.pageLimit()} * PAGE_SIZE;
    const size_t places = common + RARE_PLACES;
    const auto placeOf = [&](Entity entity) {
      const size_t index = sc::ecs2::indexOf(entity);
      return index < common ? index : std::min(common + (sc::ecs2::INDEX_LIMIT - 1 - index), places);
    };
    // One more for what is out of place.
    std::vector<std::atomic<uint32_t>> visits(places + 1);
    std::atomic<size_t> wrongOnThreads = 0;
    std::atomic<size_t> calls = 0;
    world.parallelEach<Terms...>([&](Entity entity, const auto &...components) {
      ++calls;
      wrongOnThreads += !world.valid(entity) || ((components.owner != entity) || ... || false);
      ++visits[placeOf(entity)];
    });
    REQUIRE(wrongOnThreads == 0);
    REQUIRE(calls == expected.size());
    REQUIRE(visits[places] == 0);
    size_t once = 0;
    for (const uint64_t entity : expected) {
      once += visits[placeOf(Entity{entity})] == 1;
    }
    REQUIRE(once == expected.size());
  }

} // namespace

// Gamma as well, so that the loops below go through columns of both kinds.
template <> inline constexpr bool sc::ecs2::rareComponent<Player> = true;
template <> inline constexpr bool sc::ecs2::rareComponent<Gamma> = true;

TEST_CASE("World Entities", "[ecs2][world]") {
  sc::threading::init();
  World world;

  SECTION("An Entity Exists Until It Is Destroyed") {
    const Entity first = world.create();
    const Entity second = world.create();
    REQUIRE(first != second);
    REQUIRE(world.valid(first));
    REQUIRE(world.valid(second));
    REQUIRE_FALSE(world.valid(sc::ecs2::null));
    REQUIRE_FALSE(world.valid(sc::ecs2::makeEntity(sc::ecs2::indexOf(first), 7)));

    REQUIRE(world.destroy(first));
    REQUIRE_FALSE(world.valid(first));
    REQUIRE(world.valid(second));
    REQUIRE_FALSE(world.destroy(first));
  }

  SECTION("An Index Comes Back With Another Generation") {
    const Entity first = world.create();
    world.emplace<Position>(first, 1.0f, 2.0f);
    world.destroy(first);

    const Entity again = world.create();
    REQUIRE(sc::ecs2::indexOf(again) == sc::ecs2::indexOf(first));
    REQUIRE(again != first);
    REQUIRE(world.valid(again));
    REQUIRE_FALSE(world.valid(first));
    // Nothing of the earlier entity is left.
    REQUIRE_FALSE(world.contains<Position>(again));
  }

  SECTION("There Are Never More Entities Than INDEX_LIMIT") {
    // The rare entities take their pages from the same indices.
    const Entity special = world.create(sc::ecs2::rare);
    REQUIRE(sc::ecs2::indexOf(special) == sc::ecs2::INDEX_LIMIT - PAGE_SIZE);

    Entity last = sc::ecs2::null;
    for (uint32_t i = 0; i < sc::ecs2::INDEX_LIMIT - PAGE_SIZE; ++i) {
      last = world.create();
    }
    REQUIRE(sc::ecs2::indexOf(last) == sc::ecs2::INDEX_LIMIT - PAGE_SIZE - 1);
    world.emplace<Position>(last, 1.0f, 2.0f);
    REQUIRE(world.get<Position>(last).y == 2.0f);

    REQUIRE_THROWS_AS(world.create(), std::length_error);
    REQUIRE_THROWS_AS(world.createLocal(), std::length_error);
    // The page of the rare entities is not full yet.
    Entity lastRare = special;
    for (uint32_t i = 1; i < PAGE_SIZE; ++i) {
      lastRare = world.create(sc::ecs2::rare);
    }
    REQUIRE(sc::ecs2::indexOf(lastRare) == sc::ecs2::INDEX_LIMIT - 1);
    REQUIRE_THROWS_AS(world.create(sc::ecs2::rare), std::length_error);
    REQUIRE(world.valid(special));
    REQUIRE(world.valid(lastRare));
    // Nothing is lost by trying.
    REQUIRE(world.valid(last));
    REQUIRE_FALSE(world.contains<Position>(sc::ecs2::null));
    size_t found = 0;
    world.each<const Position>([&](const Position &) { ++found; });
    REQUIRE(found == 1);

    // A destroyed entity makes room for another one of its kind.
    REQUIRE(world.destroy(last));
    REQUIRE_THROWS_AS(world.create(sc::ecs2::rare), std::length_error);
    REQUIRE(sc::ecs2::indexOf(world.create()) == sc::ecs2::INDEX_LIMIT - PAGE_SIZE - 1);
  }

  SECTION("Rare Entities Have Indices Of Their Own") {
    const Entity common = world.create();
    const Entity first = world.create(sc::ecs2::rare);
    const Entity second = world.create(sc::ecs2::rare);
    REQUIRE(sc::ecs2::indexOf(common) < PAGE_SIZE);
    REQUIRE(sc::ecs2::indexOf(first) >= sc::ecs2::INDEX_LIMIT - PAGE_SIZE);
    REQUIRE(sc::ecs2::indexOf(second) == sc::ecs2::indexOf(first) + 1);
    REQUIRE(world.valid(first));
    REQUIRE(world.valid(second));
    // Between the two ends there is nothing.
    REQUIRE_FALSE(world.valid(sc::ecs2::makeEntity(sc::ecs2::INDEX_LIMIT / 2, 0)));
    REQUIRE_FALSE(world.valid(sc::ecs2::makeEntity(sc::ecs2::INDEX_LIMIT, 0)));

    // An index goes back to the kind of entity it came from.
    REQUIRE(world.destroy(first));
    REQUIRE_FALSE(world.valid(first));
    REQUIRE(sc::ecs2::indexOf(world.create()) == sc::ecs2::indexOf(common) + 1);
    const Entity again = world.create(sc::ecs2::rare);
    REQUIRE(sc::ecs2::indexOf(again) == sc::ecs2::indexOf(first));
    REQUIRE(again != first);

    REQUIRE(world.destroy(common));
    REQUIRE(sc::ecs2::indexOf(world.create(sc::ecs2::rare)) == sc::ecs2::indexOf(second) + 1);
    REQUIRE(sc::ecs2::indexOf(world.create()) == sc::ecs2::indexOf(common));
  }

  SECTION("Entities Created Locally Are Apart From The Earlier Ones") {
    const Entity earlier = world.create();
    const Entity local = world.createLocal();
    const Entity next = world.createLocal();
    const auto pageOf = [](Entity entity) { return sc::ecs2::indexOf(entity) / PAGE_SIZE; };
    REQUIRE(pageOf(local) != pageOf(earlier));
    REQUIRE(pageOf(next) == pageOf(local));
    REQUIRE(world.valid(local));

    // Not in a page that has entities from before.
    world.sealLocal();
    const Entity later = world.createLocal();
    REQUIRE(pageOf(later) != pageOf(local));
    REQUIRE(pageOf(later) != pageOf(earlier));

    // What was left of the page is not lost.
    std::unordered_set<uint32_t> indices{sc::ecs2::indexOf(earlier), sc::ecs2::indexOf(local), sc::ecs2::indexOf(next),
                                         sc::ecs2::indexOf(later)};
    world.sealLocal();
    bool reused = false;
    for (uint32_t i = 0; i < 3 * PAGE_SIZE; ++i) {
      const Entity entity = world.create();
      REQUIRE(indices.insert(sc::ecs2::indexOf(entity)).second);
      reused = reused || pageOf(entity) == pageOf(local);
    }
    REQUIRE(reused);
  }

  SECTION("Every Thread Creates Other Entities") {
    constexpr size_t perChunk = 3000;
    constexpr size_t chunks = 64;
    std::vector<Entity> created(perChunk * chunks);
    // Some indices to hand out again.
    std::vector<Entity> earlier;
    for (int i = 0; i < 5000; ++i) {
      earlier.push_back(world.create());
    }
    for (const Entity entity : earlier) {
      world.destroy(entity);
    }

    sc::threading::parallelFor(chunks, 1, [&](size_t first, size_t last) {
      for (size_t chunk = first; chunk < last; ++chunk) {
        for (size_t i = 0; i < perChunk; ++i) {
          created[chunk * perChunk + i] = world.create();
        }
      }
    });

    std::unordered_set<uint32_t> indices;
    for (const Entity entity : created) {
      REQUIRE(world.valid(entity));
      indices.insert(sc::ecs2::indexOf(entity));
    }
    REQUIRE(indices.size() == created.size());
    for (const Entity entity : earlier) {
      REQUIRE_FALSE(world.valid(entity));
    }
  }
}

TEST_CASE("World Components", "[ecs2][world]") {
  sc::threading::init();
  World world;

  SECTION("Added, Read, Changed And Removed") {
    const Entity entity = world.create();
    REQUIRE_FALSE(world.contains<Position>(entity));
    Position &position = world.emplace<Position>(entity, 1.0f, 2.0f);
    REQUIRE(world.contains<Position>(entity));
    REQUIRE(&world.get<Position>(entity) == &position);
    world.get<Position>(entity).x = 5.0f;
    REQUIRE(world.get<const Position>(entity).x == 5.0f);
    STATIC_REQUIRE(std::is_same_v<decltype(world.get<const Position>(entity)), const Position &>);

    world.emplace<Tag>(entity);
    REQUIRE(world.contains<Tag>(entity));
    REQUIRE(world.erase<Tag>(entity));
    REQUIRE_FALSE(world.erase<Tag>(entity));

    REQUIRE(world.erase<Position>(entity));
    REQUIRE_FALSE(world.contains<Position>(entity));
    REQUIRE(world.valid(entity));
  }

  SECTION("A Component Stays Where It Is") {
    std::vector<Entity> entities;
    for (size_t i = 0; i < 3 * PAGE_SIZE; ++i) {
      entities.push_back(world.create());
      world.emplace<Position>(entities.back(), static_cast<float>(i), 0.0f);
    }
    const Position *kept = &world.get<Position>(entities[PAGE_SIZE + 7]);
    for (size_t i = 0; i < entities.size(); ++i) {
      if (i != PAGE_SIZE + 7) {
        world.destroy(entities[i]);
      }
    }
    REQUIRE(&world.get<Position>(entities[PAGE_SIZE + 7]) == kept);
    REQUIRE(kept->x == static_cast<float>(PAGE_SIZE + 7));
  }

  SECTION("A Page Has Memory Only While It Holds A Component") {
    std::vector<Entity> entities;
    for (size_t i = 0; i < 4 * PAGE_SIZE; ++i) {
      entities.push_back(world.create());
    }
    auto &positions = world.column<Position>();
    REQUIRE(positions.pages() == 0);

    // One entity of the third page.
    world.emplace<Position>(entities[2 * PAGE_SIZE + 5]);
    REQUIRE(positions.pages() == 1);
    for (const Entity entity : entities) {
      if (!world.contains<Position>(entity)) {
        world.emplace<Position>(entity);
      }
    }
    REQUIRE(positions.pages() == 4);

    // A hole as large as a page.
    for (size_t i = PAGE_SIZE; i < 2 * PAGE_SIZE; ++i) {
      world.erase<Position>(entities[i]);
    }
    REQUIRE(positions.pages() == 3);
    size_t visited = 0;
    world.each<const Position>([&](const Position &) { ++visited; });
    REQUIRE(visited == 3 * PAGE_SIZE);

    for (const Entity entity : entities) {
      world.destroy(entity);
    }
    REQUIRE(positions.pages() == 0);
  }
}

TEST_CASE("World Loops", "[ecs2][world][threading]") {
  sc::threading::init();
  World world;

  std::vector<Entity> entities;
  for (size_t i = 0; i < 3 * PAGE_SIZE + 200; ++i) {
    entities.push_back(world.create());
    world.emplace<Position>(entities.back(), static_cast<float>(i), 0.0f);
    if (i % 3 != 0) {
      world.emplace<Velocity>(entities.back(), 1.0f, 2.0f);
    }
    if (i % 5 == 0) {
      world.emplace<Tag>(entities.back());
    }
  }

  SECTION("Components Are Written And Read") {
    world.each<Position, const Velocity>([](Position &pos, const Velocity &vel) { pos.y += vel.dy; });
    world.parallelEach<Position, const Velocity>([](Position &pos, const Velocity &vel) { pos.y += vel.dy; });
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.get<Position>(entities[i]).y == (i % 3 != 0 ? 4.0f : 0.0f));
      REQUIRE(world.get<Position>(entities[i]).x == static_cast<float>(i));
    }
  }

  SECTION("Excluded Components And Components Without Members") {
    world.each<Position, Exclude<Velocity>>([](Position &pos) { pos.y += 1.0f; });
    // The function does not get a component that has no members.
    world.each<const Tag, Position>([](Position &pos) { pos.y += 10.0f; });
    world.parallelEach<Position, const Velocity, Exclude<Tag>>(
        [](Position &pos, const Velocity &) { pos.y += 100.0f; });
    for (size_t i = 0; i < entities.size(); ++i) {
      const float expected =
          (i % 3 == 0 ? 1.0f : 0.0f) + (i % 5 == 0 ? 10.0f : 0.0f) + (i % 3 != 0 && i % 5 != 0 ? 100.0f : 0.0f);
      REQUIRE(world.get<Position>(entities[i]).y == expected);
    }
  }

  SECTION("Rare Entities And Rare Components Are Gone Through Like The Others") {
    std::vector<Entity> players;
    for (int i = 0; i < 300; ++i) {
      players.push_back(world.create(sc::ecs2::rare));
      world.emplace<Player>(players.back(), i);
      world.emplace<Position>(players.back(), -1.0f, 0.0f);
      if (i % 2 == 0) {
        world.emplace<Velocity>(players.back(), 1.0f, 2.0f);
      }
    }
    // The components of all of them are in one page.
    REQUIRE(world.column<Player>().pages() == 1);
    REQUIRE(world.column<Position>().pages() == 4 + 1);

    size_t positions = 0;
    world.each<const Position>([&](const Position &) { ++positions; });
    REQUIRE(positions == entities.size() + players.size());

    world.each<Position, const Velocity>([](Position &pos, const Velocity &vel) { pos.y += vel.dy; });
    world.parallelEach<Position, const Velocity>([](Position &pos, const Velocity &vel) { pos.y += vel.dy; });
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.get<Position>(entities[i]).y == (i % 3 != 0 ? 4.0f : 0.0f));
    }
    for (size_t i = 0; i < players.size(); ++i) {
      REQUIRE(world.get<Position>(players[i]).y == (i % 2 == 0 ? 4.0f : 0.0f));
      REQUIRE(world.get<Player>(players[i]).number == static_cast<int>(i));
    }

    std::atomic<int> sum = 0;
    std::atomic<size_t> wrong = 0;
    world.parallelEach<const Player, const Position, Exclude<Velocity>>(
        [&](Entity entity, const Player &player, const Position &pos) {
          sum += player.number;
          wrong += !world.valid(entity) || pos.x != -1.0f;
        },
        1);
    // The odd numbers below 300.
    REQUIRE(sum == 150 * 150);
    REQUIRE(wrong == 0);

    for (const Entity player : players) {
      REQUIRE(world.destroy(player));
    }
    REQUIRE(world.column<Player>().pages() == 0);
    REQUIRE(world.column<Position>().pages() == 4);
  }

  SECTION("A Loop Changes Other Components Of Its Entity") {
    world.parallelEach<const Position>([&](Entity entity, const Position &pos) {
      if (static_cast<size_t>(pos.x) % 2 == 0) {
        world.emplace<Alpha>(entity, entity);
      }
      world.erase<Velocity>(entity);
    });
    REQUIRE(world.column<Velocity>().pages() == 0);
    size_t alphas = 0;
    world.each<const Alpha, const Position>([&](Entity entity, const Alpha &alpha, const Position &pos) {
      alphas += alpha.owner == entity && static_cast<size_t>(pos.x) % 2 == 0;
    });
    REQUIRE(alphas == (entities.size() + 1) / 2);
  }
}

TEST_CASE("World Loops Go Through The Entities Of A Model, Whatever Happened Before", "[ecs2][world][threading]") {
  sc::threading::init();

  const uint32_t seed = GENERATE(1u, 2u, 3u);
  std::mt19937 random{seed};
  const auto below = [&](size_t bound) { return std::uniform_int_distribution<size_t>{0, bound - 1}(random); };

  World world;
  Model model;
  std::vector<Entity> alive;

  size_t rares = 0;
  const auto spawn = [&](bool common = false) {
    // Now and then a rare one, as long as the test has room for them.
    const bool rare = !common && rares < RARE_PLACES && below(4) == 0;
    rares += rare;
    const Entity entity = rare ? world.create(sc::ecs2::rare) : world.create();
    REQUIRE(model.emplace(static_cast<uint64_t>(entity), 0u).second);
    alive.push_back(entity);
    return entity;
  };
  const auto toggle = [&](Entity entity) {
    unsigned &components = model.at(static_cast<uint64_t>(entity));
    switch (below(4)) {
    case 0:
      (components & ALPHA) != 0 ? void(world.erase<Alpha>(entity)) : void(world.emplace<Alpha>(entity, entity));
      components ^= ALPHA;
      break;
    case 1:
      (components & BETA) != 0 ? void(world.erase<Beta>(entity)) : void(world.emplace<Beta>(entity, entity));
      components ^= BETA;
      break;
    case 2:
      (components & GAMMA) != 0 ? void(world.erase<Gamma>(entity)) : void(world.emplace<Gamma>(entity, entity));
      components ^= GAMMA;
      break;
    default:
      (components & TAG) != 0 ? void(world.erase<Tag>(entity)) : void(world.emplace<Tag>(entity));
      components ^= TAG;
      break;
    }
  };
  const auto check = [&] {
    requireLoop<Alpha>(world, model, ALPHA, 0);
    requireLoop<Alpha, const Beta>(world, model, ALPHA | BETA, 0);
    requireLoop<Beta, Alpha, const Gamma>(world, model, ALPHA | BETA | GAMMA, 0);
    requireLoop<const Tag, Gamma>(world, model, TAG | GAMMA, 0);
    requireLoop<Alpha, Exclude<Beta>>(world, model, ALPHA, BETA);
    requireLoop<Gamma, const Beta, Exclude<Tag, Alpha>>(world, model, GAMMA | BETA, TAG | ALPHA);
    for (const Entity entity : alive) {
      if (!world.valid(entity)) {
        FAIL("an entity of the model does not exist");
      }
    }
  };

  // Full pages to begin with.
  for (size_t i = 0; i < 4 * PAGE_SIZE + 300; ++i) {
    const Entity entity = spawn(true);
    world.emplace<Alpha>(entity, entity);
    world.emplace<Beta>(entity, entity);
    world.emplace<Gamma>(entity, entity);
    model.at(static_cast<uint64_t>(entity)) = ALPHA | BETA | GAMMA;
  }
  check();

  for (int round = 0; round < 40; ++round) {
    CAPTURE(seed, round);
    // Few changes at first, which leave most pages full, then many.
    const size_t changes = round < 15 ? 1 + below(4) : 1 + below(alive.size() / 3);
    for (size_t change = 0; change < changes; ++change) {
      const size_t index = below(alive.size());
      switch (below(8)) {
      case 0:
        REQUIRE(world.destroy(alive[index]));
        model.erase(static_cast<uint64_t>(alive[index]));
        alive[index] = alive.back();
        alive.pop_back();
        break;
      case 1: {
        const Entity entity = spawn();
        for (int component = 0; component < 3; ++component) {
          toggle(entity);
        }
        break;
      }
      default:
        toggle(alive[index]);
        break;
      }
    }
    // Now and then a hole of several pages.
    if (round % 10 == 9) {
      const uint32_t from = static_cast<uint32_t>(below(3)) * PAGE_SIZE;
      for (size_t i = 0; i < alive.size();) {
        const uint32_t index = sc::ecs2::indexOf(alive[i]);
        if (index >= from && index < from + 2 * PAGE_SIZE) {
          world.destroy(alive[i]);
          model.erase(static_cast<uint64_t>(alive[i]));
          alive[i] = alive.back();
          alive.pop_back();
        } else {
          ++i;
        }
      }
    }
    check();
  }
}
