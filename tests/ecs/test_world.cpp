#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
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

#include "sc/ecs/world.hpp"
#include "sc/threading/threading.hpp"

namespace {

  using sc::ecs::Emplaced;
  using sc::ecs::Entity;
  using Allocator = sc::ecs::internal::EntityAllocator;
  using sc::ecs::Exclude;
  using sc::ecs::PAGE_SIZE;
  using sc::ecs::Pages;
  using sc::ecs::World;

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
  template<typename... Terms>
  void requireLoop(World &world, const Model &model, unsigned wanted, unsigned unwanted) {
    std::vector<uint64_t> expected;
    for (const auto &[entity, components]: model) {
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

    // A place for every entity in the pages of the model, which lie
    // anywhere in their areas.
    std::vector<uint32_t> pages;
    for (const auto &[entity, components]: model) {
      pages.push_back(sc::ecs::indexOf(Entity{entity}) >> sc::ecs::PAGE_BITS);
    }
    std::ranges::sort(pages);
    pages.erase(std::ranges::unique(pages).begin(), pages.end());
    const size_t places = pages.size() * PAGE_SIZE;
    const auto placeOf = [&](Entity entity) {
      const uint32_t index = sc::ecs::indexOf(entity);
      const auto page = std::ranges::lower_bound(pages, index >> sc::ecs::PAGE_BITS);
      if (page == pages.end() || *page != index >> sc::ecs::PAGE_BITS) {
        return places;
      }
      return (static_cast<size_t>(page - pages.begin()) * PAGE_SIZE) + (index & (PAGE_SIZE - 1));
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
    for (const uint64_t entity: expected) {
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

  // The pages of a column that hold components, without the spares it keeps.
  template<typename Column>
  size_t usedPages(const Column &column) {
    return column.pages() - column.spares();
  }

} // namespace

// Gamma as well, so that the loops below go through columns of both kinds.
template<>
inline constexpr bool sc::ecs::rareComponent<Player> = true;
template<>
inline constexpr bool sc::ecs::rareComponent<Gamma> = true;

TEST_CASE("World Entities", "[ecs][world]") {
  sc::threading::init();
  World world{GENERATE(Pages::Auto, Pages::Huge, Pages::Small)};

  SECTION("An Entity Exists Until It Is Destroyed") {
    const Entity first = world.create();
    const Entity second = world.create();
    REQUIRE(first != second);
    REQUIRE(world.valid(first));
    REQUIRE(world.valid(second));
    REQUIRE_FALSE(world.valid(sc::ecs::null));
    REQUIRE_FALSE(world.valid(sc::ecs::makeEntity(sc::ecs::indexOf(first), 7)));

    REQUIRE(world.destroy(first));
    REQUIRE_FALSE(world.valid(first));
    REQUIRE(world.valid(second));
    REQUIRE_FALSE(world.destroy(first));
  }

  SECTION("The Entities That Exist Are Counted") {
    REQUIRE(world.alive() == 0);
    std::vector<Entity> common;
    for (int i = 0; i < 10; ++i) {
      common.push_back(world.create());
    }
    const Entity special = world.create(sc::ecs::rare);
    REQUIRE(world.alive() == 11);

    REQUIRE(world.destroy(common[3]));
    REQUIRE(world.destroy(special));
    REQUIRE_FALSE(world.destroy(special));
    REQUIRE(world.alive() == 9);
    // Indices given back count again once they are handed out again.
    common[3] = world.create();
    REQUIRE(world.alive() == 10);

    // Created all at once over a page border, partly with an index given
    // back.
    REQUIRE(world.destroy(common[5]));
    std::vector<Entity> batch(PAGE_SIZE + 100);
    world.create(std::span<Entity>{batch});
    REQUIRE(world.alive() == 9 + batch.size());

    // Destroyed all at once; one given twice and one gone already are left
    // out.
    std::vector<Entity> gone(batch.begin(), batch.begin() + 200);
    gone.push_back(gone.front());
    gone.push_back(common[5]);
    REQUIRE(world.destroy(std::span<const Entity>{gone}) == 200);
    REQUIRE(world.alive() == 9 + batch.size() - 200);
  }

  SECTION("An Index Comes Back With Another Generation") {
    const Entity first = world.create();
    world.emplace<Position>(first, 1.0f, 2.0f);
    world.destroy(first);

    const Entity again = world.create();
    REQUIRE(sc::ecs::indexOf(again) == sc::ecs::indexOf(first));
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

  SECTION("An Area Goes On In The Next Free Page Until There Is None") {
    // Four pages: the areas start at 0, 2, 1 and 3, as the first four of all
    // pages would at 0, 2^19, 2^18 and 3 * 2^18.
    Allocator entities{2};
    const auto pageOf = [](Entity entity) { return sc::ecs::indexOf(entity) >> sc::ecs::PAGE_BITS; };
    const Entity common = entities.create();
    const Entity special = entities.create(sc::ecs::rare);
    const Entity first = entities.create(Allocator::FIRST_AREA);
    REQUIRE(pageOf(common) == 0);
    REQUIRE(pageOf(special) == 2);
    REQUIRE(pageOf(first) == 1);

    // The common area runs into the pages of the other two and goes on in
    // the last one.
    Entity last = common;
    for (uint32_t i = 0; i < PAGE_SIZE; ++i) {
      last = entities.create();
    }
    REQUIRE(pageOf(last) == 3);
    REQUIRE(entities.pageCount() == 4);
    // None is left for another area, nor for the first one once its page
    // is full.
    REQUIRE_THROWS_AS(entities.create(Allocator::FIRST_AREA + 1), std::length_error);
    for (uint32_t i = 1; i < PAGE_SIZE; ++i) {
      (void) entities.create(Allocator::FIRST_AREA);
    }
    REQUIRE_THROWS_AS(entities.create(Allocator::FIRST_AREA), std::length_error);
    // Nothing is lost by trying.
    REQUIRE(entities.valid(first));
    REQUIRE(entities.valid(last));
    REQUIRE(entities.valid(special));
    REQUIRE(entities.alive() == (2 * size_t{PAGE_SIZE}) + 2);

    // A destroyed entity makes room for another one of its area, and only
    // there.
    entities.destroy(first);
    REQUIRE_THROWS_AS(entities.create(Allocator::FIRST_AREA + 1), std::length_error);
    const Entity again = entities.create(Allocator::FIRST_AREA);
    REQUIRE(sc::ecs::indexOf(again) == sc::ecs::indexOf(first));
    REQUIRE_FALSE(entities.valid(first));
    REQUIRE_THROWS_AS(entities.create(Allocator::FIRST_AREA), std::length_error);
  }

  SECTION("Entities Created With The Same Components Lie In An Area Of Their Own") {
    const auto pageOf = [](Entity entity) { return sc::ecs::indexOf(entity) >> sc::ecs::PAGE_BITS; };
    // Within the first pages of the area that starts at the page.
    const auto inArea = [&](Entity entity, uint32_t start) { return pageOf(entity) - start < 3; };
    const Entity plain = world.create();
    const Entity moving = world.create(Position{.x = 1.0f, .y = 2.0f}, Velocity{.dx = 3.0f, .dy = 4.0f});
    const Entity turned = world.create(Velocity{.dx = 5.0f, .dy = 6.0f}, Position{.x = 7.0f, .y = 8.0f});
    const Entity still = world.create(Position{});
    const Entity special = world.create(sc::ecs::rare);
    // The areas in the order of the van der Corput sequence, whatever the
    // order of the components.
    REQUIRE(pageOf(plain) == 0);
    REQUIRE(pageOf(special) == 1u << 19);
    REQUIRE(pageOf(moving) == 1u << 18);
    REQUIRE(pageOf(turned) == pageOf(moving));
    REQUIRE(pageOf(still) == 3u << 18);
    REQUIRE(world.pageCount() == 4);
    REQUIRE(world.get<Position>(turned).x == 7.0f);
    REQUIRE(world.get<Velocity>(turned).dx == 5.0f);
    REQUIRE_FALSE(world.contains<Velocity>(still));

    // Named as template arguments, the set only picks the area: the entity
    // gets no component yet.
    const Entity planned = world.create<Velocity, Position>();
    REQUIRE(pageOf(planned) == pageOf(moving));
    REQUIRE_FALSE(world.contains<Position>(planned));
    REQUIRE_FALSE(world.contains<Velocity>(planned));
    world.emplace<Position>(planned, 9.0f, 9.0f);
    REQUIRE(world.get<Position>(planned).x == 9.0f);
    // A set of its own, the 5th area, which starts at 2^17: the indices it
    // gives back stay out of the areas the checks below take from.
    std::vector<Entity> plannedMany(PAGE_SIZE + 10);
    world.create<Tag>(std::span<Entity>{plannedMany});
    size_t inFifth = 0;
    for (const Entity entity: plannedMany) {
      inFifth += static_cast<size_t>(inArea(entity, 1u << 17) && world.valid(entity));
    }
    REQUIRE(inFifth == plannedMany.size());
    REQUIRE(world.destroy(planned));
    REQUIRE(world.destroy(std::span<const Entity>{plannedMany}) == plannedMany.size());

    // All at once, the same set goes to the same area.
    std::vector<Entity> many(PAGE_SIZE + 10);
    world.create(std::span<Entity>{many}, Velocity{}, Position{});
    size_t inMoving = 0;
    for (const Entity entity: many) {
      inMoving += static_cast<size_t>(inArea(entity, 1u << 18));
    }
    REQUIRE(inMoving == many.size());

    // A destroyed index comes back in its own area only.
    REQUIRE(world.destroy(moving));
    REQUIRE(inArea(world.create(Position{}), 3u << 18));
    REQUIRE(pageOf(world.create()) == 0);
    const Entity again = world.create(Position{}, Velocity{});
    REQUIRE(sc::ecs::indexOf(again) == sc::ecs::indexOf(moving));

    // The same after a destroy of many, on one thread and on the pool.
    const std::vector<Entity> gone(many.begin(), many.begin() + 100);
    REQUIRE(world.destroy(std::span<const Entity>{gone}) == gone.size());
    size_t back = 0;
    for (size_t i = 0; i < gone.size(); ++i) {
      back += static_cast<size_t>(inArea(world.create(Position{}, Velocity{}), 1u << 18));
    }
    REQUIRE(back == gone.size());
    const std::vector<Entity> goneOnPool(many.begin() + 100, many.end());
    REQUIRE(world.parallelDestroy(goneOnPool) == goneOnPool.size());
    REQUIRE(pageOf(world.create(Position{})) == pageOf(still));
    back = 0;
    for (size_t i = 0; i < goneOnPool.size(); ++i) {
      back += static_cast<size_t>(inArea(world.create(Velocity{}, Position{}), 1u << 18));
    }
    REQUIRE(back == goneOnPool.size());
    REQUIRE(world.alive() == 8 + many.size());
  }

  SECTION("Threads Create Entities Of Several Sets Without Sharing A Page") {
    constexpr size_t perChunk = 2000;
    constexpr size_t chunks = 32;
    std::vector<Entity> moving(perChunk * chunks);
    std::vector<Entity> still(perChunk * chunks);
    sc::threading::parallelFor(chunks, 1, [&](size_t first, size_t last) {
      for (size_t chunk = first; chunk < last; ++chunk) {
        for (size_t i = 0; i < perChunk; ++i) {
          moving[(chunk * perChunk) + i] = world.create(Position{}, Velocity{});
          still[(chunk * perChunk) + i] = world.create(Position{});
        }
      }
    });

    // The set of every page, which must be the only one in it.
    std::unordered_map<uint32_t, int> setOfPage;
    std::unordered_set<uint32_t> indices;
    size_t mixed = 0;
    const auto record = [&](Entity entity, int set) {
      REQUIRE(world.valid(entity));
      indices.insert(sc::ecs::indexOf(entity));
      mixed += setOfPage.emplace(sc::ecs::indexOf(entity) >> sc::ecs::PAGE_BITS, set).first->second != set;
    };
    for (const Entity entity: moving) {
      record(entity, 1);
    }
    for (const Entity entity: still) {
      record(entity, 2);
    }
    REQUIRE(mixed == 0);
    REQUIRE(indices.size() == moving.size() + still.size());
    REQUIRE(world.alive() == indices.size());
    size_t withVelocity = 0;
    world.each<const Position, const Velocity>([&](const Position &, const Velocity &) { ++withVelocity; });
    REQUIRE(withVelocity == moving.size());
  }

  SECTION("Rare Entities Have Indices Of Their Own") {
    const Entity common = world.create();
    const Entity first = world.create(sc::ecs::rare);
    const Entity second = world.create(sc::ecs::rare);
    REQUIRE(sc::ecs::indexOf(common) < PAGE_SIZE);
    REQUIRE(sc::ecs::indexOf(first) >> sc::ecs::PAGE_BITS == Allocator::startOf(Allocator::RARE_AREA));
    REQUIRE(sc::ecs::indexOf(second) == sc::ecs::indexOf(first) + 1);
    REQUIRE(world.valid(first));
    REQUIRE(world.valid(second));
    // Between the areas there is nothing.
    REQUIRE_FALSE(world.valid(sc::ecs::makeEntity(sc::ecs::INDEX_LIMIT / 2, 0)));
    REQUIRE_FALSE(world.valid(sc::ecs::makeEntity(sc::ecs::INDEX_LIMIT, 0)));

    // An index goes back to the kind of entity it came from.
    REQUIRE(world.destroy(first));
    REQUIRE_FALSE(world.valid(first));
    REQUIRE(sc::ecs::indexOf(world.create()) == sc::ecs::indexOf(common) + 1);
    const Entity again = world.create(sc::ecs::rare);
    REQUIRE(sc::ecs::indexOf(again) == sc::ecs::indexOf(first));
    REQUIRE(again != first);

    REQUIRE(world.destroy(common));
    REQUIRE(sc::ecs::indexOf(world.create(sc::ecs::rare)) == sc::ecs::indexOf(second) + 1);
    REQUIRE(sc::ecs::indexOf(world.create()) == sc::ecs::indexOf(common));
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
    for (const Entity entity: earlier) {
      world.destroy(entity);
    }

    sc::threading::parallelFor(chunks, 1, [&](size_t first, size_t last) {
      for (size_t chunk = first; chunk < last; ++chunk) {
        for (size_t i = 0; i < perChunk; ++i) {
          created[(chunk * perChunk) + i] = world.create();
        }
      }
    });

    std::unordered_set<uint32_t> indices;
    for (const Entity entity: created) {
      REQUIRE(world.valid(entity));
      indices.insert(sc::ecs::indexOf(entity));
    }
    REQUIRE(indices.size() == created.size());
    for (const Entity entity: earlier) {
      REQUIRE_FALSE(world.valid(entity));
    }
    // With what the threads have left of their pages not among them.
    REQUIRE(world.alive() == created.size());
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
          // TODO: under TSan this spin can keep go.store below from getting
          // TSan's lock on go for minutes (seen 2026-10-22: the main thread in
          // FutexWait of __tsan_atomic8_store, the 8 threads here; runs of 15
          // s and of over 300 s, which hit meson's timeout). std::latch or a
          // yield in the loop would leave it the lock.
          while (!go.load(std::memory_order_acquire)) {
          }
          for (size_t i = 0; i < perThread; ++i) {
            created[(t * perThread) + i] = world.create();
            createdRare[(t * perThread) + i] = world.create(sc::ecs::rare);
          }
        });
      }
      go.store(true, std::memory_order_release);
    }

    std::unordered_set<uint32_t> indices;
    for (const Entity entity: created) {
      REQUIRE(world.valid(entity));
      indices.insert(sc::ecs::indexOf(entity));
    }
    for (const Entity entity: createdRare) {
      REQUIRE(world.valid(entity));
      indices.insert(sc::ecs::indexOf(entity));
    }
    REQUIRE(indices.size() == created.size() + createdRare.size());
    REQUIRE(world.alive() == created.size() + createdRare.size());
  }
}

TEST_CASE("World Components", "[ecs][world]") {
  sc::threading::init();
  World world{GENERATE(Pages::Auto, Pages::Huge, Pages::Small)};

  SECTION("Entities Are Created All At Once With Values Of Their Own") {
    // Over three pages, in the area of their set.
    std::vector<Entity> entities((2 * size_t{PAGE_SIZE}) + 100);
    const auto alive = std::make_shared<int>(0);
    world.create<Position, Named, Tag>(entities, [&](size_t i) {
      return std::tuple{Position{.x = static_cast<float>(i), .y = 1.0f},
                        Named{.name = std::to_string(i), .alive = alive}, Tag{}};
    });
    size_t right = 0;
    for (size_t i = 0; i < entities.size(); ++i) {
      right += static_cast<size_t>(world.valid(entities[i]) && world.contains<Tag>(entities[i]) &&
                                   world.get<const Position>(entities[i]).x == static_cast<float>(i) &&
                                   world.get<const Named>(entities[i]).name == std::to_string(i));
    }
    REQUIRE(right == entities.size());
    REQUIRE(alive.use_count() == 1 + static_cast<long>(entities.size()));
    const Entity same = world.create(Tag{}, Position{}, Named{.name = "", .alive = alive});
    REQUIRE(sc::ecs::indexOf(same) >> sc::ecs::PAGE_BITS == sc::ecs::indexOf(entities.back()) >> sc::ecs::PAGE_BITS);

    // A make that throws in the second page: the first one keeps its
    // components, the second gets none, and what was made of it is gone.
    std::vector<Entity> more((2 * size_t{PAGE_SIZE}) + 100);
    const auto counted = std::make_shared<int>(0);
    const auto make = [&](size_t i) {
      if (i == PAGE_SIZE + 5) {
        throw std::runtime_error("make");
      }
      return std::tuple{Named{.name = "", .alive = counted}, Velocity{}};
    };
    const auto createMore = [&] { world.create<Named, Velocity>(more, make); };
    REQUIRE_THROWS_AS(createMore(), std::runtime_error);
    REQUIRE(counted.use_count() == 1 + static_cast<long>(PAGE_SIZE));
    size_t first = 0;
    size_t second = 0;
    for (size_t i = 0; i < more.size(); ++i) {
      REQUIRE(world.valid(more[i]));
      (i < PAGE_SIZE ? first : second) +=
          static_cast<size_t>(world.contains<Named>(more[i]) && world.contains<Velocity>(more[i]));
    }
    REQUIRE(first == PAGE_SIZE);
    REQUIRE(second == 0);
  }

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
    const size_t count =
        (sc::ecs::HUGE_PAGE_COLUMN_BYTES / sizeof(Position)) + (static_cast<unsigned long>(3 * PAGE_SIZE)) + 17;
    std::vector<Entity> entities(count);
    world.create(entities, Position{.x = 1.0f, .y = 2.0f}, Tag{}, Player{7});

    std::vector<Entity> sorted = entities;
    std::ranges::sort(sorted);
    REQUIRE(std::ranges::adjacent_find(sorted) == sorted.end());
    size_t right = 0;
    for (const Entity entity: entities) {
      right += static_cast<size_t>(
          world.valid(entity) && world.contains<Tag>(entity) && world.get<const Position>(entity).x == 1.0f &&
          world.get<const Position>(entity).y == 2.0f && world.get<const Player>(entity).number == 7);
    }
    REQUIRE(right == count);
    for (size_t i = 0; i < gone.size(); ++i) {
      REQUIRE(world.valid(gone[i]) == (i % 3 != 0));
    }

    size_t positions = 0;
    world.each<const Position>([&](const Position &) { ++positions; });
    REQUIRE(positions == count + gone.size() - ((gone.size() + 2) / 3));
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

  SECTION("Indices Given Back In Rows Come Back In Rows") {
    std::vector<Entity> made(2 * size_t{PAGE_SIZE});
    world.create(std::span<Entity>{made}, Position{.x = 9.0f, .y = 9.0f});
    // A row across the border of two pages, and two on their own.
    std::vector<Entity> gone(made.begin() + (PAGE_SIZE - 100), made.begin() + (PAGE_SIZE + 100));
    gone.push_back(made[7]);
    gone.push_back(made[3]);
    REQUIRE(world.destroy(std::span<const Entity>{gone}) == gone.size());

    std::vector<Entity> again(gone.size() + 10);
    world.create(std::span<Entity>{again}, Position{.x = 1.0f, .y = 2.0f});
    std::vector<uint32_t> given;
    std::vector<uint32_t> taken;
    for (size_t i = 0; i < gone.size(); ++i) {
      given.push_back(sc::ecs::indexOf(gone[i]));
      taken.push_back(sc::ecs::indexOf(again[i]));
    }
    std::ranges::sort(given);
    std::ranges::sort(taken);
    REQUIRE(taken == given);

    size_t right = 0;
    for (const Entity entity: again) {
      right += static_cast<size_t>(world.valid(entity) && world.get<const Position>(entity).x == 1.0f &&
                                   world.get<const Position>(entity).y == 2.0f);
    }
    REQUIRE(right == again.size());
    size_t old = 0;
    size_t fresh = 0;
    world.each<const Position>([&](const Position &position) {
      old += static_cast<size_t>(position.x == 9.0f);
      fresh += static_cast<size_t>(position.x == 1.0f);
    });
    REQUIRE(old == made.size() - gone.size());
    REQUIRE(fresh == again.size());
  }

  SECTION("A Column Moves To Huge Pages Once It Is Large") {
    // As many entities as it takes, and their components are still there.
    const size_t count = (sc::ecs::HUGE_PAGE_COLUMN_BYTES / sizeof(Position)) + PAGE_SIZE;
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
      kept += static_cast<size_t>(world.get<const Position>(entities[i]).x == static_cast<float>(i));
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
      std::byte bytes[sc::ecs::HUGE_PAGE_COMPONENT_BYTES];
    };
    static_assert(sizeof(Position) < sc::ecs::HUGE_PAGE_COMPONENT_BYTES);
    REQUIRE(world.column<Large>().hugePages() == (world.pages() == Pages::Huge || world.pages() == Pages::Auto));
    // A rare component gets small pages either way.
    REQUIRE_FALSE(world.column<Player>().hugePages());
  }

  SECTION("Threads Add And Remove Components Of Entities In The Same Page") {
    std::vector<Entity> entities(PAGE_SIZE);
    for (Entity &entity: entities) {
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
    for (Entity &entity: entities) {
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
        for (const Entity entity: entities) {
          added += world.tryEmplace<Named>(entity, std::to_string(chunk), alive) == Emplaced::Made;
          tagged += world.tryEmplace<Tag>(entity) == Emplaced::Made;
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
    REQUIRE(world.tryEmplace<Named>(entities[1], "late", alive) == Emplaced::There);

    std::atomic<size_t> removed = 0;
    sc::threading::parallelFor(chunks, 1, [&](size_t first, size_t last) {
      for (size_t chunk = first; chunk < last; ++chunk) {
        for (const Entity entity: entities) {
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
    REQUIRE(usedPages(world.column<Named>()) == 0);
    // Counted as many as there are.
    size_t tags = 0;
    world.each<const Tag>([&](Entity) { ++tags; });
    REQUIRE(world.column<Tag>().page(0).count + world.column<Tag>().page(1).count == tags);
    for (const Entity entity: entities) {
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
          for (const Entity entity: entities) {
            if ((chunk + static_cast<size_t>(round)) % 2 == 0) {
              made += world.tryEmplace<Named>(entity, std::string(100, 'm'), alive) == Emplaced::Made;
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
    for (const Entity entity: entities) {
      world.erase<Named>(entity);
    }
    REQUIRE(alive.use_count() == 1);
    REQUIRE(usedPages(world.column<Named>()) == 0);

    // One that cannot be made is not there for the next to try.
    REQUIRE_THROWS_AS(world.tryEmplace<Fragile>(entities[0], "no", true), std::runtime_error);
    REQUIRE_FALSE(world.contains<Fragile>(entities[0]));
    REQUIRE(usedPages(world.column<Fragile>()) == 0);
    REQUIRE(world.tryEmplace<Fragile>(entities[0], "yes", false) == Emplaced::Made);
    REQUIRE(world.get<Fragile>(entities[0]).text == "yes");
  }

  SECTION("Threads Replace The Same Components While Others Give And Take Them") {
    const auto alive = std::make_shared<int>(0);
    // Few entities, so that the threads keep meeting at the same ones.
    std::vector<Entity> entities(64);
    for (Entity &entity: entities) {
      entity = world.create();
    }

    constexpr size_t chunks = 16;
    sc::threading::parallelFor(chunks, 1, [&](size_t first, size_t last) {
      for (size_t chunk = first; chunk < last; ++chunk) {
        for (int round = 0; round < 200; ++round) {
          for (const Entity entity: entities) {
            switch ((chunk + static_cast<size_t>(round)) % 3) {
              case 0:
                world.emplaceOrReplace<Named>(entity, std::string(100, 'r'), alive);
                world.emplaceOrReplace<Tag>(entity);
                // erase of it takes no making bit: the page must keep its
                // memory by the count alone.
                world.emplaceOrReplace<Position>(entity, 1.0f, 2.0f);
                break;
              case 1:
                world.tryEmplace<Named>(entity, std::string(100, 't'), alive);
                world.tryEmplace<Position>(entity, 1.0f, 2.0f);
                break;
              default:
                world.erase<Named>(entity);
                world.erase<Tag>(entity);
                world.erase<Position>(entity);
                break;
            }
          }
        }
      }
    });
    // Every component there is a whole one, and the count says how many.
    size_t named = 0;
    world.each<const Named>([&](const Named &component) {
      named += component.name == std::string(100, 'r') || component.name == std::string(100, 't');
    });
    REQUIRE(alive.use_count() == 1 + static_cast<long>(named));
    REQUIRE(world.column<Named>().page(0).count == named);
    size_t tags = 0;
    world.each<const Tag>([&](Entity) { ++tags; });
    REQUIRE(world.column<Tag>().page(0).count == tags);
    size_t positions = 0;
    world.each<const Position>(
        [&](const Position &position) { positions += position.x == 1.0f && position.y == 2.0f; });
    REQUIRE(world.column<Position>().page(0).count == positions);
    for (const Entity entity: entities) {
      world.erase<Named>(entity);
      world.erase<Position>(entity);
    }
    REQUIRE(alive.use_count() == 1);
    REQUIRE(usedPages(world.column<Named>()) == 0);
    REQUIRE(usedPages(world.column<Position>()) == 0);
  }

  SECTION("A Component Is Modified In Place") {
    const Entity entity = world.create();
    const Entity without = world.create();
    world.emplace<Position>(entity, 1.0f, 2.0f);
    const Position *const before = &world.get<Position>(entity);
    REQUIRE(world.modify<Position>(entity, [](Position &position) { position.x += 10.0f; }));
    REQUIRE(world.get<Position>(entity).x == 11.0f);
    REQUIRE(&world.get<Position>(entity) == before);

    // No call for an entity without it.
    bool called = false;
    REQUIRE_FALSE(world.modify<Position>(without, [&](Position &) { called = true; }));
    REQUIRE_FALSE(called);
    REQUIRE_FALSE(world.modify<Velocity>(entity, [&](Velocity &) { called = true; }));
    REQUIRE_FALSE(called);

    // A function that throws leaves what it changed, and the component
    // unlocked for the next one.
    REQUIRE_THROWS_AS(world.modify<Position>(entity,
                                             [](Position &position) {
                                               position.y = 5.0f;
                                               throw std::runtime_error("modify");
                                             }),
                      std::runtime_error);
    REQUIRE(world.get<Position>(entity).y == 5.0f);
    REQUIRE(world.modify<Position>(entity, [](Position &position) { position.y += 1.0f; }));
    REQUIRE(world.get<Position>(entity).y == 6.0f);
    REQUIRE(world.column<Position>().page(0).count == 1);

    // One that owns something.
    const auto alive = std::make_shared<int>(0);
    world.emplace<Named>(entity, "named", alive);
    REQUIRE(world.modify<Named>(entity, [](Named &named) { named.name += " again"; }));
    REQUIRE(world.get<Named>(entity).name == "named again");
    REQUIRE(alive.use_count() == 2);
  }

  SECTION("Threads Modify The Same Components Without Losing A Change") {
    // Few, so that the threads keep meeting at the same ones.
    std::vector<Entity> targets(8);
    for (Entity &target: targets) {
      target = world.create();
      world.emplace<Position>(target);
    }
    // And some that come and go meanwhile, which owns something as well.
    const auto alive = std::make_shared<int>(0);
    std::vector<Entity> others(64);
    for (Entity &other: others) {
      other = world.create();
    }

    constexpr size_t chunks = 16;
    constexpr int rounds = 2000;
    std::atomic<size_t> missed = 0;
    sc::threading::parallelFor(chunks, 1, [&](size_t first, size_t last) {
      for (size_t chunk = first; chunk < last; ++chunk) {
        for (int round = 0; round < rounds; ++round) {
          for (const Entity target: targets) {
            world.modify<Position>(target, [](Position &position) { position.x += 1.0f; });
          }
          const Entity other = others[(chunk + static_cast<size_t>(round)) % others.size()];
          switch ((chunk + static_cast<size_t>(round)) % 3) {
            case 0:
              world.tryEmplace<Named>(other, "n", alive);
              world.tryEmplace<Velocity>(other, 1.0f, 0.0f);
              break;
            case 1:
              missed += !world.modify<Named>(other, [](Named &named) { named.name += "m"; });
              world.modify<Velocity>(other, [](Velocity &velocity) { velocity.dy += 1.0f; });
              break;
            default:
              world.erase<Named>(other);
              world.erase<Velocity>(other);
              break;
          }
        }
      }
    });
    // Every change of every thread is there.
    for (const Entity target: targets) {
      REQUIRE(world.get<Position>(target).x == static_cast<float>(chunks * rounds));
    }
    size_t named = 0;
    world.each<const Named>([&](const Named &) { ++named; });
    REQUIRE(alive.use_count() == 1 + static_cast<long>(named));
    REQUIRE(world.column<Named>().page(0).count == named);
    for (const Entity other: others) {
      world.erase<Named>(other);
      world.erase<Velocity>(other);
    }
    REQUIRE(alive.use_count() == 1);
    REQUIRE(usedPages(world.column<Named>()) == 0);
  }

  SECTION("Threads Make The Same And Other Columns At The Same Time") {
    // Each thread makes all of them, in its own order: every one is made
    // once, and every thread gets that one.
    constexpr size_t chunks = 16;
    std::vector<std::array<const void *, 4>> got(chunks);
    sc::threading::parallelFor(chunks, 1, [&](size_t first, size_t last) {
      for (size_t chunk = first; chunk < last; ++chunk) {
        for (size_t k = 0; k < 4; ++k) {
          switch ((chunk + k) % 4) {
            case 0:
              got[chunk][0] = &world.column<Position>();
              break;
            case 1:
              got[chunk][1] = &world.column<Velocity>();
              break;
            case 2:
              got[chunk][2] = &world.column<Named>();
              break;
            default:
              got[chunk][3] = &world.column<Tag>();
              break;
          }
        }
      }
    });
    for (const auto &each: got) {
      REQUIRE(each == got.front());
    }
    REQUIRE(got.front()[0] == world.findColumn<Position>());
    REQUIRE(got.front()[3] == world.findColumn<Tag>());
    // Every one is in the list destroy goes through once.
    const Entity entity = world.create();
    world.emplace<Position>(entity);
    world.emplace<Tag>(entity);
    world.destroy(entity);
    REQUIRE_FALSE(world.contains<Position>(entity));
  }

  SECTION("Threads Add Or Modify The Same Components Without Losing A Change") {
    // None has the component at first: one thread makes it, the others add.
    std::vector<Entity> targets(8);
    for (Entity &target: targets) {
      target = world.create();
    }
    constexpr size_t chunks = 16;
    constexpr int rounds = 2000;
    std::atomic<size_t> made = 0;
    sc::threading::parallelFor(chunks, 1, [&](size_t first, size_t last) {
      for (size_t chunk = first; chunk < last; ++chunk) {
        for (int round = 0; round < rounds; ++round) {
          for (const Entity target: targets) {
            made += world.emplaceOrModify<Position>(
                        target, [](Position &position) { position.x += 1.0f; }, 1.0f) == Emplaced::Made;
          }
        }
      }
    });
    REQUIRE(made == targets.size());
    for (const Entity target: targets) {
      REQUIRE(world.get<Position>(target).x == static_cast<float>(chunks * rounds));
    }
  }

  SECTION("Threads Add The First And Remove The Last Component Of A Page") {
    // So few that the page gets its memory and gives it back all the time.
    constexpr size_t chunks = 8;
    std::vector<Entity> entities(chunks);
    for (Entity &entity: entities) {
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
    // The page is a spare now.
    REQUIRE(world.column<Position>().pages() == 1);
    REQUIRE(world.column<Position>().spares() == 1);
    REQUIRE_FALSE(world.column<Position>().page(0));

    world.emplace<Position>(entities[0], 3.0f, 4.0f);
    REQUIRE(world.column<Position>().pages() == 1);
    REQUIRE(world.column<Position>().spares() == 0);
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
      REQUIRE(usedPages(owning.column<Named>()) == 0);

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
      const Entity special = owning.create(sc::ecs::rare);
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

      // Many destroyed at once give theirs back as well; one given twice and
      // one destroyed before are left out.
      std::vector<Entity> gone(batch.begin(), batch.begin() + (PAGE_SIZE / 2));
      gone.push_back(gone.front());
      gone.push_back(first);
      REQUIRE(owning.destroy(std::span<const Entity>{gone}) == PAGE_SIZE / 2);
      batch.erase(batch.begin(), batch.begin() + (PAGE_SIZE / 2));
      REQUIRE(alive.use_count() == 1 + static_cast<long>(entities.size() / 2) + 1 + static_cast<long>(batch.size()));
      REQUIRE_FALSE(owning.valid(gone.front()));
      REQUIRE(owning.get<Named>(batch.front()).name == name);

      std::atomic<size_t> found = 0;
      owning.parallelEach<const Named>([&](const Named &named) { found += named.name.size() >= 100; }, 1);
      REQUIRE(found == (entities.size() / 2) + 1 + batch.size());
    }
    // Whatever was left went with the world.
    REQUIRE(alive.use_count() == 1);
  }

  SECTION("A Component That Cannot Be Made Is Not There") {
    const Entity entity = world.create();
    REQUIRE_THROWS_AS(world.emplace<Fragile>(entity, std::string(100, 'f'), true), std::runtime_error);
    REQUIRE_FALSE(world.contains<Fragile>(entity));
    REQUIRE(usedPages(world.column<Fragile>()) == 0);
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
    REQUIRE(usedPages(world.column<Fragile>()) == 0);
  }

  SECTION("Added, Read, Changed And Removed") {
    const Entity entity = world.create();
    REQUIRE_FALSE(world.contains<Position>(entity));
    REQUIRE(world.emplace<Position>(entity, 1.0f, 2.0f) == Emplaced::Made);
    REQUIRE(world.contains<Position>(entity));
    REQUIRE(world.get<Position>(entity).y == 2.0f);
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

  SECTION("Many Entities Are Destroyed At Once On Several Threads") {
    // Enough for the pool, over several pages, some of them rare.
    const auto alive = std::make_shared<int>(0);
    std::vector<Entity> entities;
    for (size_t i = 0; i < (3 * PAGE_SIZE) + 100; ++i) {
      entities.push_back(i % 50 == 0 ? world.create(sc::ecs::rare) : world.create());
      world.emplace<Position>(entities.back(), static_cast<float>(i), 0.0f);
      if (i % 3 == 0) {
        world.emplace<Named>(entities.back(), std::string(100, 'n'), alive);
      }
    }

    // Every other one, one of them twice, one destroyed before, and nobody.
    std::vector<Entity> gone;
    std::vector<Entity> kept;
    for (size_t i = 0; i < entities.size(); ++i) {
      (i % 2 == 0 ? gone : kept).push_back(entities[i]);
    }
    const size_t freed = gone.size();
    REQUIRE(world.destroy(gone[4]));
    gone.push_back(gone[10]);
    gone.push_back(sc::ecs::null);
    REQUIRE(world.parallelDestroy(std::span<const Entity>{gone}) == freed - 1);
    REQUIRE(world.alive() == kept.size());

    for (const Entity entity: gone) {
      REQUIRE_FALSE(world.valid(entity));
    }
    size_t named = 0;
    for (const Entity entity: kept) {
      REQUIRE(world.valid(entity));
      REQUIRE(world.contains<Position>(entity));
      named += static_cast<size_t>(world.contains<Named>(entity));
    }
    REQUIRE(alive.use_count() == 1 + static_cast<long>(named));
    size_t positions = 0;
    world.each<const Position>([&](const Position &) { ++positions; });
    REQUIRE(positions == kept.size());

    // The indices come back, each once, to the kind they came from.
    std::unordered_set<uint32_t> indices;
    for (size_t i = 0; i < freed; ++i) {
      const bool rare =
          (sc::ecs::indexOf(gone[i]) >> sc::ecs::PAGE_BITS) - Allocator::startOf(Allocator::RARE_AREA) < 4;
      indices.insert(sc::ecs::indexOf(rare ? world.create(sc::ecs::rare) : world.create()));
    }
    REQUIRE(indices.size() == freed);
    for (size_t i = 0; i < freed; ++i) {
      REQUIRE(indices.contains(sc::ecs::indexOf(gone[i])));
    }
    for (const Entity entity: kept) {
      world.erase<Named>(entity);
    }
    REQUIRE(alive.use_count() == 1);

    // Many in one page are done on the calling thread, with the same result.
    World single{world.pages()};
    std::vector<Entity> page(PAGE_SIZE / 2);
    single.create(std::span<Entity>{page}, Position{});
    REQUIRE(single.parallelDestroy(std::span<const Entity>{page}) == page.size());
    REQUIRE(single.alive() == 0);
    REQUIRE(single.column<Position>().pages() == single.column<Position>().spares());
  }

  SECTION("A Component Is Looked Up Without Knowing Whether It Is There") {
    const Entity entity = world.create();
    const Entity other = world.create();
    // No column yet.
    REQUIRE(world.tryGet<Position>(entity) == nullptr);
    world.emplace<Position>(entity, 1.0f, 2.0f);
    auto *const found = world.tryGet<Position>(entity);
    REQUIRE(found == &world.get<Position>(entity));
    found->x = 3.0f;
    REQUIRE(world.get<Position>(entity).x == 3.0f);
    // A column, but not for this entity.
    REQUIRE(world.tryGet<Position>(other) == nullptr);
    REQUIRE(world.tryGet<const Position>(entity)->y == 2.0f);
    const World &read = world;
    REQUIRE(read.tryGet<Position>(entity) == found);
    REQUIRE(read.tryGet<Velocity>(entity) == nullptr);

    // A destroyed entity has none, also once another one has its index and
    // the component.
    world.destroy(entity);
    REQUIRE(world.tryGet<Position>(entity) == nullptr);
    const Entity next = world.create();
    REQUIRE(sc::ecs::indexOf(next) == sc::ecs::indexOf(entity));
    world.emplace<Position>(next);
    REQUIRE(world.tryGet<Position>(entity) == nullptr);
    REQUIRE(world.tryGet<Position>(next) != nullptr);
  }

  SECTION("A Component Is Added Or Modified") {
    const Entity entity = world.create();
    const auto grow = [](Position &position) { position.x += 1.0f; };
    REQUIRE(world.emplaceOrModify<Position>(entity, grow, 5.0f, 2.0f) == Emplaced::Made);
    // Made from the arguments, without the function.
    REQUIRE(world.get<Position>(entity).x == 5.0f);
    REQUIRE(world.emplaceOrModify<Position>(entity, grow, 9.0f) == Emplaced::Modified);
    REQUIRE(world.get<Position>(entity).x == 6.0f);
    REQUIRE(world.get<Position>(entity).y == 2.0f);
    REQUIRE(world.column<Position>().page(0).count == 1);

    // A function that throws leaves the component as the function left it,
    // and the next one gets the bit.
    REQUIRE_THROWS_AS(world.emplaceOrModify<Position>(entity, [](Position &) { throw std::runtime_error("no"); }),
                      std::runtime_error);
    REQUIRE(world.emplaceOrModify<Position>(entity, grow) == Emplaced::Modified);
    REQUIRE(world.get<Position>(entity).x == 7.0f);
  }

  SECTION("A Component Is Added Or Replaced") {
    const Entity entity = world.create();
    REQUIRE(world.emplaceOrReplace<Position>(entity, 1.0f, 2.0f) == Emplaced::Made);
    const Position *const added = &world.get<Position>(entity);
    REQUIRE(added->x == 1.0f);
    REQUIRE(world.emplaceOrReplace<Position>(entity, 3.0f) == Emplaced::Replaced);
    REQUIRE(&world.get<Position>(entity) == added);
    // Made from the arguments, not changed member by member.
    REQUIRE(world.get<Position>(entity).x == 3.0f);
    REQUIRE(world.get<Position>(entity).y == 0.0f);
    REQUIRE(world.column<Position>().page(0).count == 1);

    REQUIRE(world.emplaceOrReplace<Tag>(entity) == Emplaced::Made);
    REQUIRE(world.emplaceOrReplace<Tag>(entity) == Emplaced::Replaced);
    REQUIRE(world.contains<Tag>(entity));
    REQUIRE(world.column<Tag>().page(0).count == 1);

    // The one there gives back what it owned, and stays if the new one cannot
    // be made.
    const auto alive = std::make_shared<int>(0);
    REQUIRE(world.emplaceOrReplace<Named>(entity, std::string(100, 'a'), alive) == Emplaced::Made);
    REQUIRE(world.emplaceOrReplace<Named>(entity, std::string(100, 'b'), nullptr) == Emplaced::Replaced);
    REQUIRE(alive.use_count() == 1);
    REQUIRE(world.get<Named>(entity).name == std::string(100, 'b'));

    REQUIRE(world.emplaceOrReplace<Fragile>(entity, "kept", false) == Emplaced::Made);
    REQUIRE_THROWS_AS(world.emplaceOrReplace<Fragile>(entity, "lost", true), std::runtime_error);
    REQUIRE(world.get<Fragile>(entity).text == "kept");
    REQUIRE(world.column<Fragile>().page(0).count == 1);

    // One that cannot be made where there was none leaves none, and the page
    // without memory.
    const Entity other = world.create();
    REQUIRE(world.erase<Fragile>(entity));
    REQUIRE_THROWS_AS(world.emplaceOrReplace<Fragile>(other, "no", true), std::runtime_error);
    REQUIRE_FALSE(world.contains<Fragile>(other));
    REQUIRE(usedPages(world.column<Fragile>()) == 0);

    REQUIRE(world.destroy(entity));
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
    for (size_t i = 0; i < 3 * size_t{PAGE_SIZE}; ++i) {
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

  SECTION("A Page Has Memory Only While It Holds A Component Or Is A Spare") {
    std::vector<Entity> entities;
    for (size_t i = 0; i < 4 * size_t{PAGE_SIZE}; ++i) {
      entities.push_back(world.create());
    }
    auto &positions = world.column<Position>();
    STATIC_REQUIRE(std::remove_reference_t<decltype(positions)>::SPARE_PAGES == 4);
    REQUIRE(positions.pages() == 0);

    // One entity of the third page.
    world.emplace<Position>(entities[(2 * PAGE_SIZE) + 5]);
    REQUIRE(positions.pages() == 1);
    for (const Entity entity: entities) {
      if (!world.contains<Position>(entity)) {
        world.emplace<Position>(entity);
      }
    }
    REQUIRE(positions.pages() == 4);

    // A hole as large as a page.
    for (size_t i = PAGE_SIZE; i < 2 * size_t{PAGE_SIZE}; ++i) {
      world.erase<Position>(entities[i]);
    }
    // It keeps its memory as a spare, but is not gone through.
    REQUIRE(positions.pages() == 4);
    REQUIRE(positions.spares() == 1);
    REQUIRE_FALSE(positions.page(1));
    size_t visited = 0;
    world.each<const Position>([&](const Position &) { ++visited; });
    REQUIRE(visited == 3 * PAGE_SIZE);

    // All four are spares now, until they are trimmed.
    for (const Entity entity: entities) {
      world.destroy(entity);
    }
    REQUIRE(positions.pages() == 4);
    REQUIRE(positions.spares() == 4);
    positions.trim();
    REQUIRE(positions.pages() == 0);
    REQUIRE(positions.spares() == 0);
  }

  SECTION("A Column Keeps The Last Pages That Were Emptied") {
    using Column = sc::ecs::internal::Column<Position>;
    // On the heap: a column holds the headers of all pages.
    const auto made = std::make_unique<Column>(world.pages());
    Column &column = *made;
    constexpr uint32_t pages = Column::SPARE_PAGES + 2;
    for (uint32_t number = 0; number < pages; ++number) {
      column.emplace(sc::ecs::makeEntity(number * PAGE_SIZE, 0), static_cast<float>(number), 0.0f);
    }
    for (uint32_t number = 0; number < pages; ++number) {
      REQUIRE(column.erase(sc::ecs::makeEntity(number * PAGE_SIZE, 0)));
    }
    // The first two gave their memory back.
    REQUIRE(column.pages() == Column::SPARE_PAGES);
    REQUIRE(column.spares() == Column::SPARE_PAGES);

    // A spare that gets a component again keeps its memory, it is not
    // given back when it comes out of the spares.
    column.emplace(sc::ecs::makeEntity((pages - 1) * PAGE_SIZE, 0), 1.0f, 2.0f);
    REQUIRE(column.spares() == Column::SPARE_PAGES - 1);
    for (uint32_t number = 0; number < Column::SPARE_PAGES; ++number) {
      column.emplace(sc::ecs::makeEntity(number * PAGE_SIZE, 0), 0.0f, 0.0f);
      REQUIRE(column.erase(sc::ecs::makeEntity(number * PAGE_SIZE, 0)));
    }
    REQUIRE(column.get((pages - 1) * PAGE_SIZE).y == 2.0f);
    REQUIRE(usedPages(column) == 1);
    column.trim();
    REQUIRE(column.pages() == 1);
    REQUIRE(column.get((pages - 1) * PAGE_SIZE).y == 2.0f);

    // Large components keep fewer pages, but at least one.
    struct Large {
      std::byte bytes[1024];
    };
    STATIC_REQUIRE(sc::ecs::internal::Column<Large>::SPARE_PAGES == 1);
    STATIC_REQUIRE(sc::ecs::internal::Column<sc::ecs::Entity>::SPARE_PAGES == 4);
    STATIC_REQUIRE(sc::ecs::internal::Column<Tag>::SPARE_PAGES == 0);
  }

  SECTION("Threads Empty And Fill More Pages Than There Are Spares") {
    // Each thread an entity of a page of its own, so that pages come out of
    // the spares while their thread gives them a component again.
    constexpr size_t chunks = 16;
    std::vector<Entity> entities(chunks * PAGE_SIZE);
    world.create(std::span<Entity>{entities});
    const auto alive = std::make_shared<int>(0);
    std::atomic<size_t> wrong = 0;
    sc::threading::parallelFor(chunks, 1, [&](size_t first, size_t last) {
      for (size_t chunk = first; chunk < last; ++chunk) {
        const Entity entity = entities[chunk * PAGE_SIZE];
        for (int round = 1; round <= 5000; ++round) {
          world.emplace<Named>(entity, std::string(100, static_cast<char>('a' + chunk)), alive);
          wrong += world.get<Named>(entity).name != std::string(100, static_cast<char>('a' + chunk));
          wrong += !world.erase<Named>(entity);
        }
      }
    });
    REQUIRE(wrong == 0);
    REQUIRE(alive.use_count() == 1);
    REQUIRE(usedPages(world.column<Named>()) == 0);
    REQUIRE(world.column<Named>().pages() <= world.column<Named>().SPARE_PAGES);
    world.column<Named>().trim();
    REQUIRE(world.column<Named>().pages() == 0);
  }

  SECTION("A Global Value Is Made Once, Assigned After, And Goes With The World") {
    struct Settings {
      int level;
      std::shared_ptr<int> alive;
    };
    const auto alive = std::make_shared<int>(0);
    {
      World other;
      REQUIRE(other.tryGlobal<Settings>() == nullptr);
      REQUIRE_THROWS_AS(other.global<Settings>(), std::logic_error);
      auto &made = other.setGlobal<Settings>(1, alive);
      REQUIRE(alive.use_count() == 2);
      // Assigned: where it is stays.
      auto &again = other.setGlobal<Settings>(2, alive);
      REQUIRE(&again == &made);
      REQUIRE(alive.use_count() == 2);
      const World &seen = other;
      REQUIRE(seen.global<Settings>().level == 2);
      REQUIRE(seen.tryGlobal<Settings>() == &made);
      // The value of one world is not that of another.
      REQUIRE(world.tryGlobal<Settings>() == nullptr);
    }
    REQUIRE(alive.use_count() == 1);
  }

  SECTION("Headers And Values Get Memory Only Where Pages Are Used") {
    using Column = sc::ecs::internal::Column<Position>;
    // On the heap: a column holds the headers of all pages.
    const auto made = std::make_unique<Column>(world.pages());
    Column &column = *made;
    REQUIRE(column.headerBlocks() == 0);
    REQUIRE(column.chunks() == 0);
    // Asking makes nothing.
    REQUIRE_FALSE(column.contains(5));
    REQUIRE_FALSE(column.page(0));
    REQUIRE_FALSE(column.erase(sc::ecs::makeEntity(5, 0)));
    REQUIRE(column.headerBlocks() == 0);
    REQUIRE(column.chunks() == 0);

    // A chunk has room for a huge page and more than one block of headers.
    static_assert(Column::CHUNK_SIZE * sizeof(Position) >= sc::memory::HUGE_PAGE_SIZE);
    static_assert(Column::CHUNK_SIZE / PAGE_SIZE == Column::HEADER_BLOCK_PAGES);
    const uint32_t far = (130 * PAGE_SIZE) + 5;
    column.emplace(sc::ecs::makeEntity(far, 0), 1.0f, 2.0f);
    REQUIRE(column.headerBlocks() == 1);
    REQUIRE(column.chunks() == 1);
    // Another page of the same block and chunk.
    const uint32_t near = 129 * PAGE_SIZE;
    column.emplace(sc::ecs::makeEntity(near, 0), 3.0f, 4.0f);
    REQUIRE(column.headerBlocks() == 1);
    REQUIRE(column.chunks() == 1);
    // The last index there is.
    const uint32_t last = sc::ecs::INDEX_LIMIT - 1;
    column.emplace(sc::ecs::makeEntity(last, 0), 5.0f, 6.0f);
    REQUIRE(column.headerBlocks() == 2);
    REQUIRE(column.chunks() == 2);
    REQUIRE(column.pages() == 3);
    REQUIRE(column.get(far).y == 2.0f);
    REQUIRE(column.get(near).y == 4.0f);
    REQUIRE(column.get(last).y == 6.0f);

    // Pages that are emptied give back their memory once they are trimmed,
    // not their addresses.
    REQUIRE(column.erase(sc::ecs::makeEntity(far, 0)));
    REQUIRE(column.erase(sc::ecs::makeEntity(last, 0)));
    REQUIRE(column.pages() == 3);
    column.trim();
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
    static_assert(sc::ecs::internal::Column<Large>::CHUNK_SIZE == 1u << 16);
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
    for (size_t i = 0; i < 3 * size_t{PAGE_SIZE}; ++i) {
      entities.push_back(world.create());
      world.emplace<Position>(entities.back(), static_cast<float>(i), 0.0f);
      world.emplace<Named>(entities.back(), "a name too long for the string itself " + std::to_string(i), nullptr);
    }
    for (int round = 1; round <= 2; ++round) {
      // The middle page, all of it.
      for (size_t i = PAGE_SIZE; i < 2 * size_t{PAGE_SIZE}; ++i) {
        world.erase<Position>(entities[i]);
        world.erase<Named>(entities[i]);
      }
      REQUIRE(usedPages(world.column<Position>()) == 2);
      REQUIRE(usedPages(world.column<Named>()) == 2);
      // From the second round on with memory fetched again.
      if (round == 2) {
        world.column<Position>().trim();
        world.column<Named>().trim();
        REQUIRE(world.column<Position>().pages() == 2);
        REQUIRE(world.column<Named>().pages() == 2);
      }
      for (size_t i = PAGE_SIZE; i < 2 * size_t{PAGE_SIZE}; ++i) {
        world.emplace<Position>(entities[i], static_cast<float>(i), static_cast<float>(round));
        world.emplace<Named>(entities[i], "filled again in round " + std::to_string(round) + " " + std::to_string(i),
                             nullptr);
      }
      REQUIRE(world.column<Position>().pages() == 3);
      size_t right = 0;
      for (size_t i = 0; i < entities.size(); ++i) {
        const bool refilled = i >= PAGE_SIZE && i < 2 * size_t{PAGE_SIZE};
        const auto &pos = world.get<const Position>(entities[i]);
        const std::string &name = world.get<const Named>(entities[i]).name;
        right += static_cast<size_t>(pos.x == static_cast<float>(i) &&
                                     pos.y == (refilled ? static_cast<float>(round) : 0.0f) &&
                                     name.ends_with(" " + std::to_string(i)) && name.contains("round") == refilled);
      }
      REQUIRE(right == entities.size());
    }
  }
}

TEST_CASE("World Loops", "[ecs][world][threading]") {
  sc::threading::init();
  World world{GENERATE(Pages::Auto, Pages::Huge, Pages::Small)};

  std::vector<Entity> entities;
  for (size_t i = 0; i < (3 * PAGE_SIZE) + 200; ++i) {
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
    REQUIRE(moving == entities.size() - ((entities.size() + 2) / 3));

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
      players.push_back(world.create(sc::ecs::rare));
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

    for (const Entity player: players) {
      REQUIRE(world.destroy(player));
    }
    REQUIRE(usedPages(world.column<Player>()) == 0);
    REQUIRE(usedPages(world.column<Position>()) == 4);
  }

  SECTION("A Loop Changes Other Components Of Its Entity") {
    world.parallelEach<const Position>([&](Entity entity, const Position &pos) {
      if (static_cast<size_t>(pos.x) % 2 == 0) {
        world.emplace<Alpha>(entity, entity);
      }
      world.erase<Velocity>(entity);
    });
    REQUIRE(usedPages(world.column<Velocity>()) == 0);
    size_t alphas = 0;
    world.each<const Alpha, const Position>([&](Entity entity, const Alpha &alpha, const Position &pos) {
      alphas += alpha.owner == entity && static_cast<size_t>(pos.x) % 2 == 0;
    });
    REQUIRE(alphas == (entities.size() + 1) / 2);
  }
}

TEST_CASE("World Loops Go Through The Entities Of A Model, Whatever Happened Before", "[ecs][world][threading]") {
  sc::threading::init();

  const uint32_t seed = GENERATE(1u, 2u, 3u);
  std::mt19937 random{seed};
  const auto below = [&](size_t bound) { return std::uniform_int_distribution<size_t>{0, bound - 1}(random); };

  World world{GENERATE(Pages::Auto, Pages::Huge, Pages::Small)};
  Model model;
  std::vector<Entity> alive;

  const auto spawn = [&](bool common = false) {
    // Now and then a rare one.
    const bool rare = !common && below(4) == 0;
    const Entity entity = rare ? world.create(sc::ecs::rare) : world.create();
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
    for (const Entity entity: alive) {
      if (!world.valid(entity)) {
        FAIL("an entity of the model does not exist");
      }
    }
  };

  // Full pages to begin with.
  for (size_t i = 0; i < (4 * PAGE_SIZE) + 300; ++i) {
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
    // Now and then many destroyed at once, all over the pages; one of them
    // given twice and one destroyed before.
    if (round % 10 == 4) {
      std::vector<Entity> gone;
      for (size_t i = 0; i < alive.size();) {
        if (below(6) == 0) {
          gone.push_back(alive[i]);
          model.erase(static_cast<uint64_t>(alive[i]));
          alive[i] = alive.back();
          alive.pop_back();
        } else {
          ++i;
        }
      }
      const size_t count = gone.size();
      if (!gone.empty()) {
        gone.push_back(gone.front());
      }
      REQUIRE(world.destroy(std::span<const Entity>{gone}) == count);
      REQUIRE(world.destroy(std::span<const Entity>{gone}) == 0);
    }
    // Now and then a hole of several pages, one by one or all at once.
    if (round % 10 == 9) {
      const uint32_t from = static_cast<uint32_t>(below(3)) * PAGE_SIZE;
      std::vector<Entity> hole;
      for (size_t i = 0; i < alive.size();) {
        const uint32_t index = sc::ecs::indexOf(alive[i]);
        if (index >= from && index < from + (2 * PAGE_SIZE)) {
          hole.push_back(alive[i]);
          model.erase(static_cast<uint64_t>(alive[i]));
          alive[i] = alive.back();
          alive.pop_back();
        } else {
          ++i;
        }
      }
      if (round % 20 == 19) {
        REQUIRE(world.destroy(std::span<const Entity>{hole}) == hole.size());
      } else {
        for (const Entity entity: hole) {
          REQUIRE(world.destroy(entity));
        }
      }
    }
    check();
  }
}
