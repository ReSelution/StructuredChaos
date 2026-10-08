#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

#include "glm/glm.hpp"

#include "sc/ecs/entity.hpp"
#include "sc/ecs/registry.hpp"
#include "sc/ecs/resource.hpp"
#include "sc/stats/core.hpp"
#include "sc/threading/threading.hpp"

namespace {

  struct Position {
    float x{0.0f};
    float y{0.0f};
  };

  struct Velocity {
    float dx{0.0f};
    float dy{0.0f};
  };

  struct TransformComponent {
    glm::mat4 transform{1.0f};
  };

  struct PhysicsComponent {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    glm::vec3 velocity{0.0f};
    float mass{1.0f};
  };

  struct TagComponent {
    std::array<char, 32> name{};
  };

  struct AIStateComponent {
    int currentState{0};
    glm::vec3 targetPos{0.0f};
    float stamina{100.0f};
  };

  struct MeshComponent {
    sc::ecs::Resource<std::span<const float>> vertices;
    uint32_t meshId{0};
  };

  struct MockResource {
    bool cleared{false};
    void clear() { cleared = true; }
  };

  struct ComplexComponent {
    int id{0};
    MockResource res{};
  };

  struct MoveCopySpy {
    static inline std::atomic<size_t> copyCount{0};
    static inline std::atomic<size_t> moveCount{0};
    static inline std::atomic<size_t> ctorCount{0};

    float data[4]{0.0f};

    MoveCopySpy() { ctorCount++; }

    MoveCopySpy(const MoveCopySpy &other) {
      copyCount++;
      for (int i = 0; i < 4; ++i) {
        data[i] = other.data[i];
      }
    }

    MoveCopySpy(MoveCopySpy &&other) noexcept {
      moveCount++;
      for (int i = 0; i < 4; ++i) {
        data[i] = other.data[i];
      }
    }

    static void reset() {
      copyCount = 0;
      moveCount = 0;
      ctorCount = 0;
    }
  };

  // Index of the first entity that is not the one expected at its position, or
  // the size if there is none. A fresh registry numbers its entities from zero
  // and create() prepares them in blocks, so the entities handed out must be
  // exactly 0 .. n-1: a duplicate or an entity that was prepared but skipped
  // both show up as a gap.
  size_t first_unexpected_entity(const std::vector<entt::entity> &sorted) {
    for (size_t i = 0; i < sorted.size(); ++i) {
      if (entt::to_integral(sorted[i]) != i) {
        return i;
      }
    }
    return sorted.size();
  }

} // namespace

TEST_CASE("Registry Basic Entity and Component Operations", "[ecs][registry]") {
  sc::ecs::Registry registry;

  SECTION("Single Entity Creation and Emplace") {
    auto e = registry.create();

    registry.emplace<Position>(e, 10.0f, 20.0f);

    auto [lock, pos] = registry.get<Position>(e);
    REQUIRE(pos.x == 10.0f);
    REQUIRE(pos.y == 20.0f);
  }

  SECTION("Multiple Components on Single Entity") {
    auto e = registry.create();
    const auto ent_id = static_cast<entt::entity>(e);

    registry.emplace<Position>(ent_id, 1.0f, 2.0f);
    registry.emplace<Velocity>(ent_id, 0.5f, -0.5f);

    auto [lock, pos, vel] = registry.get<Position, Velocity>(ent_id);
    REQUIRE(pos.x == 1.0f);
    REQUIRE(vel.dx == 0.5f);
  }

  SECTION("Const Access") {
    auto e = registry.create();
    const auto ent_id = static_cast<entt::entity>(e);

    registry.emplace<Position>(ent_id, 3.0f, 4.0f);
    registry.emplace<Velocity>(ent_id, 1.5f, 2.5f);

    const auto &const_registry = registry;
    {
      auto [lock, pos] = const_registry.cget<Position>(ent_id);
      STATIC_REQUIRE(std::is_const_v<std::remove_reference_t<decltype(pos)>>);
      REQUIRE(pos.y == 4.0f);
    }
    {
      auto [lock, pos, vel] = const_registry.cget<Position, Velocity>(ent_id);
      REQUIRE(pos.x == 3.0f);
      REQUIRE(vel.dy == 2.5f);
    }
  }

  SECTION("Erase Component") {
    auto e = registry.create();
    const auto ent_id = static_cast<entt::entity>(e);

    registry.emplace<Position>(ent_id, 5.0f, 5.0f);
    registry.erase<Position>(ent_id);
  }
}

TEST_CASE("Registry Resource Components", "[ecs][registry][resource]") {
  constexpr size_t count = 2000;
  constexpr size_t vertex_count = 16;

  SECTION("Buffers Survive Storage Growth And Erase") {
    sc::ecs::Registry registry;
    std::vector<sc::ecs::Entity> entities(count);
    registry.create(entities.begin(), entities.end());

    // No reserve: the storage grows several times and moves the components.
    for (size_t i = 0; i < count; ++i) {
      std::array<float, vertex_count> vertices{};
      vertices.fill(static_cast<float>(i));
      registry.emplace<MeshComponent>(entities[i], std::span<const float>(vertices), static_cast<uint32_t>(i));
    }

    // Erasing moves the last component into the freed slot.
    for (size_t i = 0; i < count; i += 2) {
      registry.erase<MeshComponent>(entities[i]);
    }

    for (size_t i = 1; i < count; i += 2) {
      auto [lock, mesh] = registry.get<MeshComponent>(entities[i]);
      REQUIRE(mesh.meshId == i);
      REQUIRE(mesh.vertices.size() == vertex_count);
      REQUIRE(mesh.vertices[0] == static_cast<float>(i));
      REQUIRE(mesh.vertices[vertex_count - 1] == static_cast<float>(i));
    }
  }

  SECTION("More Registries Than Thread-Local Keys") {
    // A registry used to take one thread-local key per component, of which a
    // process has about a thousand.
    constexpr size_t registry_count = 1500;
    std::vector<std::unique_ptr<sc::ecs::Registry>> registries;
    registries.reserve(registry_count);

    for (size_t i = 0; i < registry_count; ++i) {
      auto &registry = *registries.emplace_back(std::make_unique<sc::ecs::Registry>());
      REQUIRE_NOTHROW(registry.reserve<Position>(1));
      REQUIRE_NOTHROW(registry.reserve<MeshComponent>(1));
    }
  }
}

TEST_CASE("Registry Access Cache", "[ecs][registry]") {
  SECTION("A New Registry At The Address Of A Destroyed One") {
    // Both registries live in the same stack slot one after the other. The
    // second one must not pick up what the thread cached for the first.
    for (int round = 0; round < 3; ++round) {
      sc::ecs::Registry registry;
      auto e = registry.create();

      registry.emplace<Position>(e, static_cast<float>(round), 0.0f);

      auto [lock, pos] = registry.get<Position>(e);
      REQUIRE(pos.x == static_cast<float>(round));
    }
  }

  SECTION("First Use Of A Component From Many Threads") {
    // Not used anywhere else, so the threads race for creating its access
    // object in this registry.
    struct FirstUseComponent {
      int value{0};
    };

    constexpr int thread_count = 8;
    constexpr int per_thread = 200;
    constexpr int entity_count = thread_count * per_thread;

    sc::ecs::Registry registry;
    std::vector<sc::ecs::Entity> entities(static_cast<size_t>(entity_count));
    registry.create(entities.begin(), entities.end());

    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < thread_count; ++t) {
      threads.emplace_back([&, t]() {
        while (!start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        for (int i = 0; i < per_thread; ++i) {
          const int index = (t * per_thread) + i;
          registry.emplace<FirstUseComponent>(entities[index], index);
        }
      });
    }
    start.store(true, std::memory_order_release);
    for (auto &thread : threads) {
      thread.join();
    }

    auto view = registry.view<FirstUseComponent>();
    REQUIRE(view.size() == entities.size());

    auto [lock, last] = registry.get<FirstUseComponent>(entities.back());
    REQUIRE(last.value == entity_count - 1);
  }

  SECTION("Two Registries Used Alternately") {
    sc::ecs::Registry first;
    sc::ecs::Registry second;
    auto e1 = first.create();
    auto e2 = second.create();

    first.emplace<Position>(e1, 1.0f, 0.0f);
    second.emplace<Position>(e2, 2.0f, 0.0f);

    for (int i = 0; i < 4; ++i) {
      {
        auto [lock, pos] = first.get<Position>(e1);
        REQUIRE(pos.x == 1.0f);
      }
      {
        auto [lock, pos] = second.get<Position>(e2);
        REQUIRE(pos.x == 2.0f);
      }
    }
  }
}

TEST_CASE("Registry Reads Of Several Components In Different Orders", "[ecs][registry]") {
  // Two readers name the same components in opposite order while writers
  // keep taking the exclusive locks. With locks taken in the order written,
  // a waiting writer could make the two readers block each other for good.
  constexpr int iterations = 20'000;

  sc::ecs::Registry registry;
  auto e = registry.create();
  registry.emplace<Position>(e, 1.0f, 2.0f);
  registry.emplace<Velocity>(e, 3.0f, 4.0f);

  std::atomic<bool> stop{false};
  std::atomic<int> mismatches{0};

  std::thread position_first([&]() {
    for (int i = 0; i < iterations; ++i) {
      auto [lock, pos, vel] = registry.get<Position, Velocity>(e);
      if (pos.x != 1.0f || vel.dx != 3.0f) {
        mismatches.fetch_add(1);
      }
    }
  });
  std::thread velocity_first([&]() {
    for (int i = 0; i < iterations; ++i) {
      auto [lock, vel, pos] = registry.get<Velocity, Position>(e);
      if (pos.x != 1.0f || vel.dx != 3.0f) {
        mismatches.fetch_add(1);
      }
    }
  });
  // Writers on another entity, so the values read above never change.
  auto other = registry.create();
  std::thread position_writer([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      registry.emplace<Position>(other, 0.0f, 0.0f);
      registry.erase<Position>(other);
    }
  });
  std::thread velocity_writer([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      registry.emplace<Velocity>(other, 0.0f, 0.0f);
      registry.erase<Velocity>(other);
    }
  });

  position_first.join();
  velocity_first.join();
  stop.store(true);
  position_writer.join();
  velocity_writer.join();

  REQUIRE(mismatches.load() == 0);
}

TEST_CASE("Registry Parallel Entity Creation", "[ecs][registry]") {
  constexpr int thread_count = 8;
  // Not a multiple of the block size, so the threads run out of prepared
  // entities at different points.
  constexpr int per_thread = 5003;

  sc::ecs::Registry registry;
  std::vector<std::vector<entt::entity>> created(thread_count);

  std::atomic<bool> start{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < thread_count; ++t) {
    threads.emplace_back([&, t]() {
      created[t].reserve(per_thread);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int i = 0; i < per_thread; ++i) {
        created[t].push_back(registry.create());
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (auto &thread : threads) {
    thread.join();
  }

  std::vector<entt::entity> all;
  for (const auto &part : created) {
    all.insert(all.end(), part.begin(), part.end());
  }
  std::ranges::sort(all);

  REQUIRE(all.size() == static_cast<size_t>(thread_count) * per_thread);
  // No entity handed out twice ...
  REQUIRE(std::ranges::adjacent_find(all) == all.end());
  // ... and none left out in between.
  REQUIRE(first_unexpected_entity(all) == all.size());
  REQUIRE(std::ranges::none_of(all, [](entt::entity e) { return e == entt::entity{entt::null}; }));
}

TEST_CASE("Registry Entity Creation From Many Threads", "[ecs][registry]") {
  // Far more threads than cores, so threads are often interrupted between
  // taking an index and reading the entity that belongs to it. Handing out an
  // entity twice in that situation is rare, hence the volume: several rounds,
  // each close to the 2^20 entities a registry can hold.
  constexpr int rounds = 8;
  constexpr int thread_count = 64;
  constexpr int per_thread = 14'000;

  for (int round = 0; round < rounds; ++round) {
    sc::ecs::Registry registry;
    std::vector<std::vector<entt::entity>> created(thread_count);

    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < thread_count; ++t) {
      threads.emplace_back([&, t]() {
        created[t].reserve(per_thread);
        ready.fetch_add(1);
        while (!start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        for (int i = 0; i < per_thread; ++i) {
          created[t].push_back(registry.create());
        }
      });
    }
    while (ready.load() != thread_count) {
      std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (auto &thread : threads) {
      thread.join();
    }

    std::vector<entt::entity> all;
    all.reserve(static_cast<size_t>(thread_count) * per_thread);
    for (const auto &part : created) {
      all.insert(all.end(), part.begin(), part.end());
    }
    std::ranges::sort(all);

    REQUIRE(all.size() == static_cast<size_t>(thread_count) * per_thread);
    // No entity handed out twice ...
    REQUIRE(std::ranges::adjacent_find(all) == all.end());
    // ... and none left out in between.
    REQUIRE(first_unexpected_entity(all) == all.size());
  }
}

TEST_CASE("Registry Holds More Than 2^20 Entities", "[ecs][registry]") {
  // The limit of 32-bit entity ids. The project builds EnTT with 64-bit ids,
  // which leaves 32 bits for the entity number.
  STATIC_REQUIRE(sizeof(entt::entity) == 8);

  constexpr size_t count = (size_t{1} << 20) + 1000;

  sc::ecs::Registry registry;
  std::vector<sc::ecs::Entity> entities(count);
  registry.create(entities.begin(), entities.end());

  std::vector<entt::entity> all(entities.begin(), entities.end());
  std::ranges::sort(all);
  REQUIRE(first_unexpected_entity(all) == all.size());

  // A component on the last entity, beyond the old limit.
  registry.emplace<Position>(entities.back(), 1.0f, 2.0f);
  auto [lock, pos] = registry.get<Position>(entities.back());
  REQUIRE(pos.y == 2.0f);
}

TEST_CASE("Registry Bulk Entity Creation", "[ecs][registry]") {
  sc::ecs::Registry registry;
  constexpr size_t count = 100;
  std::vector<sc::ecs::Entity> entities(count);

  registry.create(entities.begin(), entities.end());

  REQUIRE(entities.size() == count);

  for (size_t i = 0; i < count; ++i) {
    registry.emplace<Position>(static_cast<entt::entity>(entities[i]), static_cast<float>(i), 0.0f);
  }

  auto [lock, pos] = registry.get<Position>(static_cast<entt::entity>(entities[42]));
  REQUIRE(pos.x == 42.0f);
}

TEST_CASE("Registry PFR Resource Cleanup on Destroy", "[ecs][registry][pfr]") {
  sc::ecs::Registry registry;
  auto e = registry.create();
  const auto ent_id = static_cast<entt::entity>(e);

  registry.emplace<ComplexComponent>(ent_id, 1337, MockResource{false});

  SECTION("Cleanup via erase") { registry.erase<ComplexComponent>(ent_id); }
}

TEST_CASE("Registry Multi-Threaded Stress Test", "[ecs][registry][threading]") {
  sc::threading::init();

  constexpr size_t total_entities = 10000;
  constexpr size_t batch_size = 1000;

  sc::ecs::Registry registry;
  registry.reserve<TransformComponent>(total_entities);
  registry.reserve<PhysicsComponent>(total_entities);
  registry.reserve<TagComponent>(total_entities);
  registry.reserve<MeshComponent>(total_entities);

  std::vector<int> entitiesStart;
  for (size_t i = 0; i < total_entities; i += batch_size) {
    entitiesStart.emplace_back(static_cast<int>(i));
  }

  sc::threading::detachBatch(
      std::move(entitiesStart),
      [&registry](int start) {
        float rawData[200]{};
        for (size_t i = 0; i < batch_size; ++i) {
          auto e = registry.create();
          e.add<TransformComponent>();
          e.add<PhysicsComponent>();
          e.add<TagComponent>();
          e.add<AIStateComponent>();
          e.add<MeshComponent>(std::span<const float>(rawData, 200), static_cast<uint32_t>(start + i));
        }
      },
      nullptr);

  sc::threading::wait_until_finished();

  auto view = registry.view<PhysicsComponent>();
  REQUIRE(view.size() == total_entities);
}

TEST_CASE("Registry Range Insert and Move/Copy Behavior", "[ecs][registry][semantics]") {
  constexpr size_t batch_size = 100;

  SECTION("Copy Insert") {
    sc::ecs::Registry registry;
    MoveCopySpy::reset();

    std::vector<MoveCopySpy> spies{batch_size};
    std::vector<sc::ecs::Entity> entities{batch_size};
    registry.create(entities.begin(), entities.end());

    registry.insert<MoveCopySpy>(entities.begin(), entities.end(), spies.begin());

    REQUIRE(MoveCopySpy::copyCount.load() >= batch_size);
  }

  SECTION("Move Insert") {
    sc::ecs::Registry registry;
    MoveCopySpy::reset();

    std::vector<MoveCopySpy> spies{batch_size};
    std::vector<sc::ecs::Entity> entities{batch_size};
    registry.create(entities.begin(), entities.end());

    registry.insert<MoveCopySpy>(entities.begin(), entities.end(), std::make_move_iterator(spies.begin()));

    REQUIRE(MoveCopySpy::moveCount.load() >= batch_size);
    REQUIRE(MoveCopySpy::copyCount.load() == 0);
  }
}
