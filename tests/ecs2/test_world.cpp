#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
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
  using sc::ecs2::Pages;
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

  // Components that have something to give back at their end.
  struct Named {
    std::string name;
    // Counts the components there are.
    std::shared_ptr<int> alive;
  };

  struct Owner {
    std::unique_ptr<int> value;
  };

  // Cannot always be made.
  struct Fragile {
    Fragile(std::string text, bool fails) : text(std::move(text)) {
      if (fails) {
        throw std::runtime_error("fragile");
      }
    }
    std::string text;
  };

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

#ifdef __linux__
  // The addresses the process has reserved, used or not, in bytes.
  size_t addressSpace() {
    std::ifstream status{"/proc/self/status"};
    std::string key;
    size_t kilobytes = 0;
    while (status >> key) {
      if (key == "VmSize:") {
        status >> kilobytes;
        break;
      }
    }
    return kilobytes * 1024;
  }
#endif

} // namespace

// Gamma as well, so that the loops below go through columns of both kinds.
template <> inline constexpr bool sc::ecs2::rareComponent<Player> = true;
template <> inline constexpr bool sc::ecs2::rareComponent<Gamma> = true;

TEST_CASE("World Entities", "[ecs2][world]") {
  sc::threading::init();
  World world{GENERATE(Pages::Auto, Pages::Huge, Pages::Small)};

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
    // And what the new one gets is not the earlier one's.
    world.emplace<Position>(again, 3.0f, 4.0f);
    REQUIRE(world.contains<Position>(again));
    REQUIRE_FALSE(world.contains<Position>(first));
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

  SECTION("Threads Outside The Pool Create Other Entities") {
    constexpr size_t perThread = 20000;
    constexpr size_t threadCount = 8;
    std::vector<Entity> created(perThread * threadCount);
    std::vector<Entity> createdRare(perThread * threadCount);
    std::atomic<bool> go{false};
    {
      std::vector<std::jthread> threads;
      for (size_t t = 0; t < threadCount; ++t) {
        threads.emplace_back([&, t] {
          while (!go.load(std::memory_order_acquire)) {
          }
          for (size_t i = 0; i < perThread; ++i) {
            created[t * perThread + i] = world.create();
            createdRare[t * perThread + i] = world.create(sc::ecs2::rare);
          }
        });
      }
      go.store(true, std::memory_order_release);
    }

    std::unordered_set<uint32_t> indices;
    for (const Entity entity : created) {
      REQUIRE(world.valid(entity));
      indices.insert(sc::ecs2::indexOf(entity));
    }
    for (const Entity entity : createdRare) {
      REQUIRE(world.valid(entity));
      indices.insert(sc::ecs2::indexOf(entity));
    }
    REQUIRE(indices.size() == created.size() + createdRare.size());
  }
}

TEST_CASE("World Components", "[ecs2][world]") {
  sc::threading::init();
  World world{GENERATE(Pages::Auto, Pages::Huge, Pages::Small)};

  SECTION("Entities Are Created With Their Components All At Once") {
    // Some indices to hand out again, in no order, before the new ones.
    std::vector<Entity> gone;
    for (size_t i = 0; i < 100; ++i) {
      gone.push_back(world.create());
      world.emplace<Position>(gone.back(), 9.0f, 9.0f);
    }
    for (size_t i = 0; i < gone.size(); i += 3) {
      REQUIRE(world.destroy(gone[i]));
    }

    // Enough for the column to move to huge pages where it does that.
    const size_t count = sc::ecs2::HUGE_PAGE_COLUMN_BYTES / sizeof(Position) + 3 * PAGE_SIZE + 17;
    std::vector<Entity> entities(count);
    world.create(entities, Position{1.0f, 2.0f}, Tag{}, Player{7});

    std::vector<Entity> sorted = entities;
    std::ranges::sort(sorted);
    REQUIRE(std::ranges::adjacent_find(sorted) == sorted.end());
    size_t right = 0;
    for (const Entity entity : entities) {
      right += world.valid(entity) && world.contains<Tag>(entity) && world.get<const Position>(entity).x == 1.0f &&
               world.get<const Position>(entity).y == 2.0f && world.get<const Player>(entity).number == 7;
    }
    REQUIRE(right == count);
    for (size_t i = 0; i < gone.size(); ++i) {
      REQUIRE(world.valid(gone[i]) == (i % 3 != 0));
    }

    size_t positions = 0;
    world.each<const Position>([&](const Position &) { ++positions; });
    REQUIRE(positions == count + gone.size() - (gone.size() + 2) / 3);
    size_t tagged = 0;
    world.each<const Tag, const Player>([&](const Player &) { ++tagged; });
    REQUIRE(tagged == count);
    REQUIRE(world.column<Position>().hugePages() == (world.pages() == Pages::Huge || world.pages() == Pages::Auto));

    // An entity of the batch is one like any other.
    REQUIRE(world.erase<Position>(entities[count / 2]));
    REQUIRE(world.destroy(entities.front()));
    REQUIRE_FALSE(world.valid(entities.front()));

    world.create(std::span<Entity>{});
  }

  SECTION("A Column Moves To Huge Pages Once It Is Large") {
    // As many entities as it takes, and their components are still there.
    const size_t count = sc::ecs2::HUGE_PAGE_COLUMN_BYTES / sizeof(Position) + PAGE_SIZE;
    std::vector<Entity> entities;
    const bool small = world.pages() == Pages::Small;
    for (size_t i = 0; i < count; ++i) {
      entities.push_back(world.create());
      world.emplace<Position>(entities.back(), static_cast<float>(i), 2.0f);
      if (i == 0) {
        REQUIRE(world.column<Position>().hugePages() == (world.pages() == Pages::Huge));
      }
    }
    REQUIRE(world.column<Position>().hugePages() == !small);
    size_t kept = 0;
    for (size_t i = 0; i < count; ++i) {
      kept += world.get<const Position>(entities[i]).x == static_cast<float>(i);
    }
    REQUIRE(kept == count);
    // A rare component stays with small pages.
    REQUIRE_FALSE(world.column<Player>().hugePages());
  }

  SECTION("The Columns Are Backed With The Pages Of The World") {
    REQUIRE(World{}.pages() == Pages::Auto);
    REQUIRE(world.column<Position>().hugePages() == (world.pages() == Pages::Huge));
    // Large enough for huge pages wherever the size decides.
    struct Large {
      std::byte bytes[sc::ecs2::HUGE_PAGE_COMPONENT_BYTES];
    };
    static_assert(sizeof(Position) < sc::ecs2::HUGE_PAGE_COMPONENT_BYTES);
    REQUIRE(world.column<Large>().hugePages() == (world.pages() == Pages::Huge || world.pages() == Pages::Auto));
    // A rare component gets small pages either way.
    REQUIRE_FALSE(world.column<Player>().hugePages());
  }

  SECTION("Threads Add And Remove Components Of Entities In The Same Page") {
    std::vector<Entity> entities(PAGE_SIZE);
    for (Entity &entity : entities) {
      entity = world.create();
    }
    // Every chunk has entities all over the page, next to those of the others.
    constexpr size_t chunks = 16;
    std::atomic<size_t> wrong = 0;
    sc::threading::parallelFor(chunks, 1, [&](size_t first, size_t last) {
      for (size_t chunk = first; chunk < last; ++chunk) {
        for (int round = 0; round < 20; ++round) {
          for (size_t i = chunk; i < entities.size(); i += chunks) {
            world.emplace<Position>(entities[i], static_cast<float>(i), 1.0f);
            world.emplace<Tag>(entities[i]);
          }
          for (size_t i = chunk; i < entities.size(); i += chunks) {
            wrong += !world.contains<Tag>(entities[i]) || world.get<Position>(entities[i]).x != static_cast<float>(i);
            if (round != 19) {
              world.erase<Position>(entities[i]);
              world.erase<Tag>(entities[i]);
            }
          }
        }
      }
    });
    REQUIRE(wrong == 0);
    REQUIRE(world.column<Position>().page(0).count == PAGE_SIZE);
    REQUIRE(world.column<Tag>().page(0).count == PAGE_SIZE);
    for (size_t i = 0; i < entities.size(); ++i) {
      REQUIRE(world.get<Position>(entities[i]).x == static_cast<float>(i));
    }
  }

  SECTION("Threads Give The Same Entities A Component And Take It Away") {
    const auto alive = std::make_shared<int>(0);
    // In two pages, the second one with few of them.
    std::vector<Entity> entities(PAGE_SIZE + 50);
    for (Entity &entity : entities) {
      entity = world.create();
    }
    // Some have it already.
    for (size_t i = 0; i < entities.size(); i += 7) {
      world.emplace<Named>(entities[i], "before", alive);
      world.emplace<Tag>(entities[i]);
    }
    const size_t before = (entities.size() + 6) / 7;

    constexpr size_t chunks = 16;
    std::atomic<size_t> added = 0;
    std::atomic<size_t> tagged = 0;
    sc::threading::parallelFor(chunks, 1, [&](size_t first, size_t last) {
      for (size_t chunk = first; chunk < last; ++chunk) {
        // Every chunk goes for every entity.
        for (const Entity entity : entities) {
          added += world.tryEmplace<Named>(entity, std::to_string(chunk), alive);
          tagged += world.tryEmplace<Tag>(entity);
        }
      }
    });
    // Each of them once, and none twice.
    REQUIRE(added == entities.size() - before);
    REQUIRE(tagged == entities.size() - before);
    REQUIRE(alive.use_count() == 1 + static_cast<long>(entities.size()));
    REQUIRE(world.column<Named>().page(0).count == PAGE_SIZE);
    REQUIRE(world.column<Named>().page(1).count == 50);
    REQUIRE(world.column<Tag>().page(1).count == 50);
    for (size_t i = 0; i < entities.size(); ++i) {
      const std::string &name = world.get<Named>(entities[i]).name;
      REQUIRE((i % 7 == 0 ? name == "before" : std::stoul(name) < chunks));
    }
    REQUIRE_FALSE(world.tryEmplace<Named>(entities[1], "late", alive));

    std::atomic<size_t> removed = 0;
    sc::threading::parallelFor(chunks, 1, [&](size_t first, size_t last) {
      for (size_t chunk = first; chunk < last; ++chunk) {
        for (const Entity entity : entities) {
          removed += world.erase<Named>(entity);
          if (chunk % 2 == 0) {
            world.erase<Tag>(entity);
          } else {
            world.tryEmplace<Tag>(entity);
          }
        }
      }
    });
    REQUIRE(removed == entities.size());
    REQUIRE(alive.use_count() == 1);
    REQUIRE(world.column<Named>().pages() == 0);
    // Counted as many as there are.
    size_t tags = 0;
    world.each<const Tag>([&](Entity) { ++tags; });
    REQUIRE(world.column<Tag>().page(0).count + world.column<Tag>().page(1).count == tags);
    for (const Entity entity : entities) {
      world.erase<Tag>(entity);
    }
    REQUIRE(world.column<Tag>().pages() == 0);

    // Coming and going at the same time, also one that owns something: made
    // as often as destroyed, apart from the ones that are left.
    std::atomic<size_t> made = 0;
    std::atomic<size_t> gone = 0;
    sc::threading::parallelFor(chunks, 1, [&](size_t first, size_t last) {
      for (size_t chunk = first; chunk < last; ++chunk) {
        for (int round = 0; round < 4; ++round) {
          for (const Entity entity : entities) {
            if ((chunk + static_cast<size_t>(round)) % 2 == 0) {
              made += world.tryEmplace<Named>(entity, std::string(100, 'm'), alive);
            } else {
              gone += world.erase<Named>(entity);
            }
          }
        }
      }
    });
    size_t named = 0;
    world.each<const Named>([&](const Named &component) { named += component.name == std::string(100, 'm'); });
    REQUIRE(made - gone == named);
    REQUIRE(alive.use_count() == 1 + static_cast<long>(named));
    REQUIRE(world.column<Named>().page(0).count + world.column<Named>().page(1).count == named);
    for (const Entity entity : entities) {
      world.erase<Named>(entity);
    }
    REQUIRE(alive.use_count() == 1);
    REQUIRE(world.column<Named>().pages() == 0);

    // One that cannot be made is not there for the next to try.
    REQUIRE_THROWS_AS(world.tryEmplace<Fragile>(entities[0], "no", true), std::runtime_error);
    REQUIRE_FALSE(world.contains<Fragile>(entities[0]));
    REQUIRE(world.column<Fragile>().pages() == 0);
    REQUIRE(world.tryEmplace<Fragile>(entities[0], "yes", false));
    REQUIRE(world.get<Fragile>(entities[0]).text == "yes");
  }

  SECTION("Threads Add The First And Remove The Last Component Of A Page") {
    // So few that the page gets its memory and gives it back all the time.
    constexpr size_t chunks = 8;
    std::vector<Entity> entities(chunks);
    for (Entity &entity : entities) {
      entity = world.create();
    }
    std::atomic<size_t> wrong = 0;
    sc::threading::parallelFor(chunks, 1, [&](size_t first, size_t last) {
      for (size_t chunk = first; chunk < last; ++chunk) {
        for (int round = 1; round <= 5000; ++round) {
          world.emplace<Position>(entities[chunk], static_cast<float>(round), 1.0f);
          // Not taken away with the memory another thread gave back.
          wrong += world.get<Position>(entities[chunk]).x != static_cast<float>(round);
          wrong += !world.erase<Position>(entities[chunk]);
        }
      }
    });
    REQUIRE(wrong == 0);
    REQUIRE(world.column<Position>().pages() == 0);
    REQUIRE_FALSE(world.column<Position>().page(0));

    world.emplace<Position>(entities[0], 3.0f, 4.0f);
    REQUIRE(world.column<Position>().pages() == 1);
    REQUIRE(world.get<Position>(entities[0]).y == 4.0f);
  }

  SECTION("Components That Own Something Give It Back") {
    const auto alive = std::make_shared<int>(0);
    // Too long to be kept in the string itself.
    const std::string name(100, 'n');
    {
      World owning{world.pages()};
      const Entity first = owning.create();
      owning.emplace<Named>(first, name, alive);
      REQUIRE(owning.get<Named>(first).name == name);
      REQUIRE(alive.use_count() == 2);
      REQUIRE(owning.erase<Named>(first));
      REQUIRE(alive.use_count() == 1);
      REQUIRE(owning.column<Named>().pages() == 0);

      owning.emplace<Named>(first, name, alive);
      owning.emplace<Owner>(first, std::make_unique<int>(7));
      REQUIRE(*owning.get<Owner>(first).value == 7);
      REQUIRE(owning.destroy(first));
      REQUIRE(alive.use_count() == 1);

      // In several pages, and in the one of the rare entities.
      std::vector<Entity> entities;
      for (size_t i = 0; i < PAGE_SIZE + 500; ++i) {
        entities.push_back(owning.create());
        owning.emplace<Named>(entities.back(), name + std::to_string(i), alive);
      }
      const Entity special = owning.create(sc::ecs2::rare);
      owning.emplace<Named>(special, name, alive);
      REQUIRE(alive.use_count() == 1 + static_cast<long>(entities.size()) + 1);
      for (size_t i = 0; i < entities.size(); i += 2) {
        REQUIRE(owning.destroy(entities[i]));
      }
      REQUIRE(alive.use_count() == 1 + static_cast<long>(entities.size() / 2) + 1);
      REQUIRE(owning.get<Named>(entities[PAGE_SIZE + 1]).name == name + std::to_string(PAGE_SIZE + 1));

      // Every entity gets a copy of its own.
      std::vector<Entity> batch(PAGE_SIZE + 10);
      owning.create(std::span<Entity>{batch}, Named{.name = name, .alive = alive});
      REQUIRE(alive.use_count() == 1 + static_cast<long>(entities.size() / 2) + 1 + static_cast<long>(batch.size()));
      REQUIRE(owning.get<Named>(batch.back()).name == name);

      std::atomic<size_t> found = 0;
      owning.parallelEach<const Named>([&](const Named &named) { found += named.name.size() >= 100; }, 1);
      REQUIRE(found == entities.size() / 2 + 1 + batch.size());
    }
    // Whatever was left went with the world.
    REQUIRE(alive.use_count() == 1);
  }

  SECTION("A Component That Cannot Be Made Is Not There") {
    const Entity entity = world.create();
    REQUIRE_THROWS_AS(world.emplace<Fragile>(entity, std::string(100, 'f'), true), std::runtime_error);
    REQUIRE_FALSE(world.contains<Fragile>(entity));
    REQUIRE(world.column<Fragile>().pages() == 0);
    REQUIRE_FALSE(world.column<Fragile>().page(0));

    // Next to one that is there.
    const Entity other = world.create();
    world.emplace<Fragile>(other, std::string(100, 'o'), false);
    REQUIRE_THROWS_AS(world.emplace<Fragile>(entity, std::string(100, 'f'), true), std::runtime_error);
    REQUIRE_FALSE(world.contains<Fragile>(entity));
    REQUIRE(world.column<Fragile>().page(0).count == 1);
    REQUIRE(world.get<Fragile>(other).text == std::string(100, 'o'));

    world.emplace<Fragile>(entity, "made", false);
    REQUIRE(world.get<Fragile>(entity).text == "made");
    REQUIRE(world.erase<Fragile>(entity));
    REQUIRE(world.erase<Fragile>(other));
    REQUIRE(world.column<Fragile>().pages() == 0);
  }

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

  SECTION("A World That Is Only Read") {
    struct Unused {
      int value{0};
    };
    const Entity entity = world.create();
    world.emplace<Position>(entity, 1.0f, 2.0f);
    world.emplace<Tag>(entity);

    const World &read = world;
    REQUIRE(read.valid(entity));
    REQUIRE(read.contains<Position>(entity));
    REQUIRE(read.contains<const Tag>(entity));
    REQUIRE(read.get<Position>(entity).y == 2.0f);
    REQUIRE(&read.get<const Position>(entity) == &world.get<Position>(entity));
    STATIC_REQUIRE(std::is_same_v<decltype(read.get<Position>(entity)), const Position &>);
    STATIC_REQUIRE(std::is_same_v<decltype(read.get<const Position>(entity)), const Position &>);

    // Asking for a component makes no column for it, in no world.
    REQUIRE_FALSE(read.contains<Unused>(entity));
    REQUIRE_FALSE(world.contains<Unused>(entity));
    REQUIRE(read.findColumn<Unused>() == nullptr);
    REQUIRE(read.findColumn<Position>() == &world.column<Position>());
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

  SECTION("Headers And Values Get Memory Only Where Pages Are Used") {
    using Column = sc::ecs2::internal::Column<Position>;
    Column column{world.pages()};
    REQUIRE(column.headerBlocks() == 0);
    REQUIRE(column.chunks() == 0);
    // Asking makes nothing.
    REQUIRE_FALSE(column.contains(5));
    REQUIRE_FALSE(column.page(0));
    REQUIRE_FALSE(column.erase(5));
    REQUIRE(column.headerBlocks() == 0);
    REQUIRE(column.chunks() == 0);

    // A chunk has room for a huge page and more than one block of headers.
    static_assert(Column::CHUNK_SIZE * sizeof(Position) >= sc::memory::HUGE_PAGE_SIZE);
    static_assert(Column::CHUNK_SIZE / PAGE_SIZE == Column::HEADER_BLOCK_PAGES);
    const uint32_t far = (130 * PAGE_SIZE) + 5;
    column.emplace(far, 1.0f, 2.0f);
    REQUIRE(column.headerBlocks() == 1);
    REQUIRE(column.chunks() == 1);
    // Another page of the same block and chunk.
    const uint32_t near = 129 * PAGE_SIZE;
    column.emplace(near, 3.0f, 4.0f);
    REQUIRE(column.headerBlocks() == 1);
    REQUIRE(column.chunks() == 1);
    // The last index there is, which is a rare entity's.
    const uint32_t last = sc::ecs2::INDEX_LIMIT - 1;
    column.emplace(last, 5.0f, 6.0f);
    REQUIRE(column.headerBlocks() == 2);
    REQUIRE(column.chunks() == 2);
    REQUIRE(column.pages() == 3);
    REQUIRE(column.get(far).y == 2.0f);
    REQUIRE(column.get(near).y == 4.0f);
    REQUIRE(column.get(last).y == 6.0f);

    // Pages that are emptied give back their memory, not their addresses.
    REQUIRE(column.erase(far));
    REQUIRE(column.erase(last));
    REQUIRE(column.pages() == 1);
    REQUIRE(column.headerBlocks() == 2);
    REQUIRE(column.chunks() == 2);
    REQUIRE_FALSE(column.contains(far));
    REQUIRE(column.get(near).x == 3.0f);
  }

  SECTION("A Column Reserves The Addresses Of Its Chunks Only") {
    struct Large {
      std::byte bytes[1024];
    };
    // 64 MiB of addresses for a chunk; all indices would be 16 GiB.
    static_assert(sc::ecs2::internal::Column<Large>::CHUNK_SIZE == 1u << 16);
#ifdef __linux__
    const size_t before = addressSpace();
#endif
    std::vector<Entity> entities(10000);
    world.create(entities, Large{});
    REQUIRE(world.column<Large>().chunks() == 1);
#ifdef __linux__
    // With room for what else the process reserves meanwhile: mimalloc may
    // take a whole arena of 1 GiB.
    REQUIRE(addressSpace() - before < (size_t{2} << 30));
#endif
  }

  SECTION("A Page Emptied And Filled Again Has The New Components") {
    std::vector<Entity> entities;
    for (size_t i = 0; i < 3 * PAGE_SIZE; ++i) {
      entities.push_back(world.create());
      world.emplace<Position>(entities.back(), static_cast<float>(i), 0.0f);
      world.emplace<Named>(entities.back(), "a name too long for the string itself " + std::to_string(i), nullptr);
    }
    for (int round = 1; round <= 2; ++round) {
      // The middle page, all of it.
      for (size_t i = PAGE_SIZE; i < 2 * PAGE_SIZE; ++i) {
        world.erase<Position>(entities[i]);
        world.erase<Named>(entities[i]);
      }
      REQUIRE(world.column<Position>().pages() == 2);
      REQUIRE(world.column<Named>().pages() == 2);
      for (size_t i = PAGE_SIZE; i < 2 * PAGE_SIZE; ++i) {
        world.emplace<Position>(entities[i], static_cast<float>(i), static_cast<float>(round));
        world.emplace<Named>(entities[i], "filled again in round " + std::to_string(round) + " " + std::to_string(i),
                             nullptr);
      }
      REQUIRE(world.column<Position>().pages() == 3);
      size_t right = 0;
      for (size_t i = 0; i < entities.size(); ++i) {
        const bool refilled = i >= PAGE_SIZE && i < 2 * PAGE_SIZE;
        const Position &pos = world.get<const Position>(entities[i]);
        const std::string &name = world.get<const Named>(entities[i]).name;
        right += pos.x == static_cast<float>(i) && pos.y == (refilled ? static_cast<float>(round) : 0.0f) &&
                 name.ends_with(" " + std::to_string(i)) && name.contains("round") == refilled;
      }
      REQUIRE(right == entities.size());
    }
  }
}

TEST_CASE("World Loops", "[ecs2][world][threading]") {
  sc::threading::init();
  World world{GENERATE(Pages::Auto, Pages::Huge, Pages::Small)};

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

  SECTION("A World That Is Only Read Is Gone Through As Well") {
    struct Unused {
      int value{0};
    };
    const World &read = world;
    size_t moving = 0;
    read.each<const Position, const Velocity>([&](const Position &, const Velocity &vel) { moving += vel.dy == 2.0f; });
    REQUIRE(moving == entities.size() - (entities.size() + 2) / 3);

    std::atomic<size_t> still = 0;
    read.parallelEach<const Position, Exclude<Velocity>>([&](Entity entity, const Position &pos) {
      still += read.valid(entity) && static_cast<size_t>(pos.x) % 3 == 0;
    });
    REQUIRE(still == (entities.size() + 2) / 3);

    // A component the world never had: no entity has it, and none is kept
    // out by it. No loop makes its column.
    size_t found = 0;
    read.each<const Position, const Unused>([&](const Position &, const Unused &) { ++found; });
    read.parallelEach<const Unused>([&](const Unused &) { ++found; });
    REQUIRE(found == 0);
    read.each<const Position, Exclude<Unused>>([&](const Position &) { ++found; });
    world.each<Position, Exclude<Unused>>([&](Position &) { ++found; });
    REQUIRE(found == 2 * entities.size());
    REQUIRE(read.findColumn<Unused>() == nullptr);
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

  World world{GENERATE(Pages::Auto, Pages::Huge, Pages::Small)};
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
