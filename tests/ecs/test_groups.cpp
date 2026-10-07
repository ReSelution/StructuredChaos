#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include "sc/ecs/entity.hpp"
#include "sc/ecs/registry.hpp"

namespace {

  // Every component carries the number of its entity, so a reader can tell
  // whether the components it is handed belong together.
  struct GroupPosition {
    float owner{0.0f};
  };

  struct GroupVelocity {
    float owner{0.0f};
  };

  struct GroupHealth {
    float owner{0.0f};
  };

  float number_of(entt::entity e) { return static_cast<float>(entt::to_integral(e)); }

  std::vector<sc::ecs::Entity> make_entities(sc::ecs::Registry &registry, size_t count) {
    std::vector<sc::ecs::Entity> entities(count);
    registry.create(entities.begin(), entities.end());
    return entities;
  }

} // namespace

TEST_CASE("Registry Groups", "[ecs][registry][group]") {
  constexpr size_t count = 100;

  sc::ecs::Registry registry;
  auto entities = make_entities(registry, count);
  for (size_t i = 0; i < count; ++i) {
    registry.emplace<GroupPosition>(entities[i], number_of(entities[i]));
    if (i % 2 == 0) {
      registry.emplace<GroupVelocity>(entities[i], number_of(entities[i]));
    }
  }

  SECTION("Owning Group Over Existing Components") {
    auto group = registry.group<GroupPosition, GroupVelocity>();
    REQUIRE(group.size() == count / 2);

    size_t visited = 0;
    bool matching = true;
    group.raw().each([&](entt::entity e, const GroupPosition &position, const GroupVelocity &velocity) {
      ++visited;
      matching = matching && position.owner == number_of(e) && velocity.owner == number_of(e);
    });
    REQUIRE(visited == count / 2);
    REQUIRE(matching);
  }

  SECTION("Group Follows Later Changes") {
    {
      auto group = registry.group<GroupPosition, GroupVelocity>();
      REQUIRE(group.size() == count / 2);
    }

    registry.emplace<GroupVelocity>(entities[1], number_of(entities[1]));
    registry.erase<GroupPosition>(entities[0]);
    registry.erase<GroupVelocity>(entities[2]);

    auto group = registry.group<GroupPosition, GroupVelocity>();
    REQUIRE(group.size() == (count / 2) - 1);

    bool matching = true;
    group.raw().each([&](entt::entity e, const GroupPosition &position, const GroupVelocity &velocity) {
      matching = matching && position.owner == number_of(e) && velocity.owner == number_of(e);
    });
    REQUIRE(matching);

    // Single components stay reachable after the group moved them around.
    auto [lock, position] = registry.get<GroupPosition>(entities[1]);
    REQUIRE(position.owner == number_of(entities[1]));
  }

  SECTION("Partly Owning Group") {
    auto group = registry.group<GroupPosition>(entt::get<GroupVelocity>);
    REQUIRE(group.size() == count / 2);
  }

  SECTION("Non-Owning Group With Exclusion") {
    {
      auto group = registry.group<>(entt::get<GroupPosition>, entt::exclude<GroupVelocity>);
      REQUIRE(group.size() == count / 2);
    }

    // The group above had to go first: it holds the shared lock, and writing
    // one of its components from the same thread would wait for it forever.
    registry.emplace<GroupVelocity>(entities[1], number_of(entities[1]));

    auto again = registry.group<>(entt::get<GroupPosition>, entt::exclude<GroupVelocity>);
    REQUIRE(again.size() == (count / 2) - 1);
  }

  SECTION("View Over Components Of A Group") {
    {
      auto group = registry.group<GroupPosition, GroupVelocity>();
      REQUIRE(group.size() == count / 2);
    }
    // Both components share one lock now, which the view takes twice.
    auto view = registry.view<GroupPosition, GroupVelocity>();
    size_t visited = 0;
    view.raw().each([&](const GroupPosition &, const GroupVelocity &) { ++visited; });
    REQUIRE(visited == count / 2);
  }
}

TEST_CASE("Registry Group Under Concurrent Writes", "[ecs][registry][group][threading]") {
  // One thread adds and removes positions, another velocities, so entities
  // keep entering and leaving the group from two sides. Each change of one
  // component reorders the storage of the other; a reader that could look at
  // them in between would find components of different entities side by side.
  constexpr size_t count = 256;
  constexpr int rounds = 150;

  sc::ecs::Registry registry;
  auto entities = make_entities(registry, count);

  {
    auto group = registry.group<GroupPosition, GroupVelocity>();
    REQUIRE(group.empty());
  }

  std::atomic<bool> stop{false};
  std::atomic<long> mismatches{0};
  std::atomic<long> seen{0};

  std::thread reader([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      auto group = registry.group<GroupPosition, GroupVelocity>();
      group.raw().each([&](entt::entity e, const GroupPosition &position, const GroupVelocity &velocity) {
        seen.fetch_add(1, std::memory_order_relaxed);
        if (position.owner != number_of(e) || velocity.owner != number_of(e)) {
          mismatches.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
  });

  std::thread position_writer([&]() {
    for (int round = 0; round < rounds; ++round) {
      for (auto e : entities) {
        registry.emplace<GroupPosition>(e, number_of(e));
      }
      for (auto e : entities) {
        registry.erase<GroupPosition>(e);
      }
    }
  });
  std::thread velocity_writer([&]() {
    for (int round = 0; round < rounds; ++round) {
      for (auto e : entities) {
        registry.emplace<GroupVelocity>(e, number_of(e));
      }
      for (auto e : entities) {
        registry.erase<GroupVelocity>(e);
      }
    }
  });

  position_writer.join();
  velocity_writer.join();
  stop.store(true);
  reader.join();

  REQUIRE(mismatches.load() == 0);

  auto group = registry.group<GroupPosition, GroupVelocity>();
  REQUIRE(group.empty());
}

TEST_CASE("Registry Group Created While Its Components Are In Use", "[ecs][registry][group][threading]") {
  // The first group() call moves the components to a shared lock while other
  // threads are in the middle of taking the old ones.
  constexpr size_t count = 256;
  constexpr int rounds = 150;

  sc::ecs::Registry registry;
  auto entities = make_entities(registry, count);
  for (auto e : entities) {
    registry.emplace<GroupPosition>(e, number_of(e));
  }

  std::atomic<bool> started{false};
  std::atomic<bool> stop{false};
  std::atomic<long> mismatches{0};

  std::thread velocity_writer([&]() {
    for (int round = 0; round < rounds; ++round) {
      for (auto e : entities) {
        registry.emplace<GroupVelocity>(e, number_of(e));
      }
      started.store(true, std::memory_order_release);
      for (auto e : entities) {
        registry.erase<GroupVelocity>(e);
      }
    }
  });
  std::thread reader([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      auto view = registry.view<GroupPosition, GroupVelocity>();
      view.raw().each([&](entt::entity e, const GroupPosition &position, const GroupVelocity &velocity) {
        if (position.owner != number_of(e) || velocity.owner != number_of(e)) {
          mismatches.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
  });
  std::thread single_reader([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      for (size_t i = 0; i < count; i += 16) {
        auto [lock, position] = registry.get<GroupPosition>(entities[i]);
        if (position.owner != number_of(entities[i])) {
          mismatches.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
  });

  while (!started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  {
    auto group = registry.group<GroupPosition, GroupVelocity>();
    bool matching = true;
    group.raw().each([&](entt::entity e, const GroupPosition &position, const GroupVelocity &velocity) {
      matching = matching && position.owner == number_of(e) && velocity.owner == number_of(e);
    });
    REQUIRE(matching);
  }

  velocity_writer.join();
  stop.store(true);
  reader.join();
  single_reader.join();

  REQUIRE(mismatches.load() == 0);
}

TEST_CASE("Registry Overlapping Groups", "[ecs][registry][group][threading]") {
  // The second group shares a component with the first one, so afterwards all
  // three components have to be guarded by a single lock.
  constexpr size_t count = 128;
  constexpr int rounds = 150;

  sc::ecs::Registry registry;
  auto entities = make_entities(registry, count);
  for (auto e : entities) {
    registry.emplace<GroupVelocity>(e, number_of(e));
  }

  {
    auto first = registry.group<GroupPosition, GroupVelocity>();
    REQUIRE(first.empty());
  }
  {
    auto second = registry.group<>(entt::get<GroupVelocity, GroupHealth>);
    REQUIRE(second.empty());
  }

  std::atomic<bool> stop{false};
  std::atomic<long> mismatches{0};

  std::thread reader([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      {
        auto first = registry.group<GroupPosition, GroupVelocity>();
        first.raw().each([&](entt::entity e, const GroupPosition &position, const GroupVelocity &velocity) {
          if (position.owner != number_of(e) || velocity.owner != number_of(e)) {
            mismatches.fetch_add(1, std::memory_order_relaxed);
          }
        });
      }
      {
        auto second = registry.group<>(entt::get<GroupVelocity, GroupHealth>);
        second.raw().each([&](entt::entity e, const GroupVelocity &velocity, const GroupHealth &health) {
          if (velocity.owner != number_of(e) || health.owner != number_of(e)) {
            mismatches.fetch_add(1, std::memory_order_relaxed);
          }
        });
      }
    }
  });
  std::thread position_writer([&]() {
    for (int round = 0; round < rounds; ++round) {
      for (auto e : entities) {
        registry.emplace<GroupPosition>(e, number_of(e));
      }
      for (auto e : entities) {
        registry.erase<GroupPosition>(e);
      }
    }
  });
  std::thread health_writer([&]() {
    for (int round = 0; round < rounds; ++round) {
      for (auto e : entities) {
        registry.emplace<GroupHealth>(e, number_of(e));
      }
      for (auto e : entities) {
        registry.erase<GroupHealth>(e);
      }
    }
  });

  position_writer.join();
  health_writer.join();
  stop.store(true);
  reader.join();

  REQUIRE(mismatches.load() == 0);
}
