#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include "sc/ecs/entity.hpp"
#include "sc/ecs/registry.hpp"

namespace {

  struct DestroyPosition {
    float owner{0.0f};
  };

  struct DestroyVelocity {
    float owner{0.0f};
  };

  // Counts how often the registry asked it to give up its memory.
  struct CountingResource {
    static constexpr bool is_resource = true;
    static inline std::atomic<int> cleared{0};

    static void clear() { cleared.fetch_add(1); }
  };

  struct DestroyOwner {
    int id{0};
    CountingResource resource{};
  };

  float number_of(entt::entity e) { return static_cast<float>(entt::to_entity(e)); }

  std::vector<sc::ecs::Entity> make_entities(sc::ecs::Registry &registry, size_t count) {
    std::vector<sc::ecs::Entity> entities(count);
    registry.create(entities.begin(), entities.end());
    return entities;
  }

  template <typename... Components> size_t count_of(sc::ecs::Registry &registry) {
    auto view = registry.view<Components...>();
    size_t count = 0;
    for ([[maybe_unused]] auto e : view) {
      ++count;
    }
    return count;
  }

} // namespace

TEST_CASE("Registry Destroy", "[ecs][registry][destroy]") {
  sc::ecs::Registry registry;

  SECTION("Removes The Entity And Its Components") {
    auto entities = make_entities(registry, 10);
    for (auto e : entities) {
      registry.emplace<DestroyPosition>(e, number_of(e));
      registry.emplace<DestroyVelocity>(e, number_of(e));
    }

    REQUIRE(registry.valid(entities[3]));
    REQUIRE(registry.destroy(entities[3]));
    REQUIRE_FALSE(registry.valid(entities[3]));

    REQUIRE(count_of<DestroyPosition>(registry) == 9);
    REQUIRE(count_of<DestroyVelocity>(registry) == 9);

    // The others are untouched.
    auto [lock, position] = registry.get<DestroyPosition>(entities[4]);
    REQUIRE(position.owner == number_of(entities[4]));
  }

  SECTION("Entity Without Components") {
    auto e = registry.create();
    REQUIRE(registry.destroy(e));
    REQUIRE_FALSE(registry.valid(e));
  }

  SECTION("Destroying Twice Reports The Second Attempt") {
    auto entities = make_entities(registry, 2);
    REQUIRE(registry.destroy(entities[0]));
    REQUIRE_FALSE(registry.destroy(entities[0]));
    REQUIRE(registry.valid(entities[1]));
  }

  SECTION("A Reused Number Does Not Revive Old Handles") {
    auto entities = make_entities(registry, 4);
    const entt::entity old = entities[2];
    REQUIRE(registry.destroy(old));

    // Enough new entities for the freed number to come around again.
    auto fresh = make_entities(registry, 8);
    bool number_reused = false;
    for (auto e : fresh) {
      REQUIRE(static_cast<entt::entity>(e) != old);
      number_reused = number_reused || entt::to_entity(static_cast<entt::entity>(e)) == entt::to_entity(old);
    }
    REQUIRE(number_reused);
    REQUIRE_FALSE(registry.valid(old));
  }

  SECTION("Releases Resources Of The Components") {
    CountingResource::cleared.store(0);
    auto entities = make_entities(registry, 3);
    for (auto e : entities) {
      registry.emplace<DestroyOwner>(e);
    }

    REQUIRE(registry.destroy(entities[1]));
    REQUIRE(CountingResource::cleared.load() == 1);
  }

  SECTION("Range Of Entities") {
    auto entities = make_entities(registry, 100);
    for (auto e : entities) {
      registry.emplace<DestroyPosition>(e, number_of(e));
    }

    registry.destroy(entities.begin(), entities.begin() + 60);

    REQUIRE(count_of<DestroyPosition>(registry) == 40);
    REQUIRE_FALSE(registry.valid(entities[0]));
    REQUIRE_FALSE(registry.valid(entities[59]));
    REQUIRE(registry.valid(entities[60]));
  }

  SECTION("Entity In A Group") {
    auto entities = make_entities(registry, 20);
    for (auto e : entities) {
      registry.emplace<DestroyPosition>(e, number_of(e));
      registry.emplace<DestroyVelocity>(e, number_of(e));
    }
    {
      auto group = registry.group<DestroyPosition, DestroyVelocity>();
      REQUIRE(group.size() == 20);
    }

    REQUIRE(registry.destroy(entities[7]));

    auto group = registry.group<DestroyPosition, DestroyVelocity>();
    REQUIRE(group.size() == 19);
    bool matching = true;
    group.raw().each([&](entt::entity e, const DestroyPosition &position, const DestroyVelocity &velocity) {
      matching = matching && position.owner == number_of(e) && velocity.owner == number_of(e);
    });
    REQUIRE(matching);
  }
}

TEST_CASE("Registry Destroy From Several Threads", "[ecs][registry][destroy][threading]") {
  // Every thread destroys its own share of the entities while a reader keeps
  // walking both components and checks that they still belong together.
  constexpr int thread_count = 4;
  constexpr size_t per_thread = 2000;
  constexpr size_t survivors = 500;

  sc::ecs::Registry registry;
  auto entities = make_entities(registry, (thread_count * per_thread) + survivors);
  for (auto e : entities) {
    registry.emplace<DestroyPosition>(e, number_of(e));
    registry.emplace<DestroyVelocity>(e, number_of(e));
  }

  std::atomic<bool> stop{false};
  std::atomic<long> mismatches{0};
  std::atomic<long> failed{0};

  std::thread reader([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      auto view = registry.view<DestroyPosition, DestroyVelocity>();
      view.raw().each([&](entt::entity e, const DestroyPosition &position, const DestroyVelocity &velocity) {
        if (position.owner != number_of(e) || velocity.owner != number_of(e)) {
          mismatches.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
  });

  std::vector<std::thread> destroyers;
  for (int t = 0; t < thread_count; ++t) {
    destroyers.emplace_back([&, t]() {
      for (size_t i = 0; i < per_thread; ++i) {
        if (!registry.destroy(entities[(t * per_thread) + i])) {
          failed.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto &thread : destroyers) {
    thread.join();
  }
  stop.store(true);
  reader.join();

  REQUIRE(failed.load() == 0);
  REQUIRE(mismatches.load() == 0);
  REQUIRE(count_of<DestroyPosition>(registry) == survivors);
  REQUIRE(count_of<DestroyVelocity>(registry) == survivors);
  REQUIRE(registry.valid(entities.back()));
  REQUIRE_FALSE(registry.valid(entities.front()));
}
