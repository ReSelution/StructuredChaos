#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

#include "sc/ecs/resource.hpp"
#include "sc/ecs/world.hpp"
#include "sc/threading/threading.hpp"

namespace {

  using sc::ecs::ResourceCache;
  using sc::ecs::ResourceHandle;
  using sc::ecs::World;

  // Counts how many there are, so that a test sees when one is given back.
  struct Texture {
    static inline std::atomic<int> alive{0};

    int id;

    explicit Texture(int id) : id(id) { alive.fetch_add(1, std::memory_order_relaxed); }
    Texture(const Texture &) = delete;
    Texture &operator=(const Texture &) = delete;
    ~Texture() { alive.fetch_sub(1, std::memory_order_relaxed); }
  };

  struct Material {
    ResourceHandle<Texture> albedo;
  };

} // namespace

TEST_CASE("ECS Resource Load Once", "[ecs][resource][threading]") {
  sc::threading::init();
  Texture::alive = 0;
  {
    ResourceCache<Texture> cache;
    std::atomic<int> loads{0};
    constexpr size_t CALLS = 1000;
    std::vector<const Texture *> got(CALLS, nullptr);

    sc::threading::parallelFor(CALLS, 1, [&](size_t first, size_t last) {
      for (size_t i = first; i < last; ++i) {
        const ResourceHandle<Texture> texture = cache.load(7, [&] {
          loads.fetch_add(1, std::memory_order_relaxed);
          // Long enough for the other threads to come and wait.
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          return Texture(7);
        });
        got[i] = &*texture;
      }
    });

    CHECK(loads.load() == 1);
    for (const Texture *texture: got) {
      REQUIRE(texture == got[0]);
    }
    CHECK(got[0]->id == 7);
    CHECK(cache.size() == 1);
  }
  CHECK(Texture::alive.load() == 0);
}

TEST_CASE("ECS Resource Collect", "[ecs][resource]") {
  Texture::alive = 0;
  ResourceCache<Texture> cache;
  int loads = 0;
  const auto loader = [&] {
    ++loads;
    return Texture(1);
  };

  SECTION("only once the last handle is gone, and only in collect") {
    ResourceHandle<Texture> first = cache.load(1, loader);
    ResourceHandle<Texture> second = first;
    CHECK(cache.collect() == 0);

    first.reset();
    CHECK(cache.collect() == 0);
    second.reset();
    CHECK(Texture::alive.load() == 1);
    CHECK(cache.size() == 1);

    CHECK(cache.collect() == 1);
    CHECK(Texture::alive.load() == 0);
    CHECK(cache.size() == 0);
    CHECK(cache.collect() == 0);
  }

  SECTION("a load before collect gets it back without loading") {
    cache.load(1, loader).reset();
    ResourceHandle<Texture> again = cache.load(1, loader);
    CHECK(loads == 1);
    CHECK(cache.collect() == 0);
    CHECK(Texture::alive.load() == 1);

    // Queued anew when it goes once more.
    again.reset();
    CHECK(cache.collect() == 1);
    CHECK(Texture::alive.load() == 0);
  }

  SECTION("a load after collect loads it anew") {
    cache.load(1, loader).reset();
    CHECK(cache.collect() == 1);
    const ResourceHandle<Texture> again = cache.load(1, loader);
    CHECK(loads == 2);
    CHECK(again->id == 1);
  }

  SECTION("find gives what is there and nothing else") {
    CHECK(!cache.find(1));
    const ResourceHandle<Texture> loaded = cache.load(1, loader);
    CHECK(cache.find(1) == loaded);
  }
}

TEST_CASE("ECS Resource Force Load", "[ecs][resource]") {
  Texture::alive = 0;
  ResourceCache<Texture> cache;

  ResourceHandle<Texture> old = cache.load(1, [] { return Texture(1); });
  const ResourceHandle<Texture> fresh = cache.forceLoad(1, [] { return Texture(2); });
  CHECK(old->id == 1);
  CHECK(fresh->id == 2);
  CHECK(cache.find(1) == fresh);
  CHECK(cache.size() == 1);

  CHECK(cache.collect() == 0);
  old.reset();
  CHECK(Texture::alive.load() == 2);
  CHECK(cache.collect() == 1);
  CHECK(Texture::alive.load() == 1);
  // The old one went without taking the key of the new one along.
  CHECK(cache.find(1) == fresh);
}

TEST_CASE("ECS Resource Loader Throws", "[ecs][resource][threading]") {
  sc::threading::init();
  Texture::alive = 0;
  ResourceCache<Texture> cache;

  SECTION("on one thread") {
    CHECK_THROWS_AS(cache.load(1, []() -> Texture { throw std::runtime_error("no file"); }), std::runtime_error);
    CHECK(cache.size() == 0);
    CHECK(!cache.find(1));
    // The failed entry is given back without a destructor of a texture.
    CHECK(cache.collect() == 1);

    const ResourceHandle<Texture> texture = cache.load(1, [] { return Texture(1); });
    CHECK(texture->id == 1);
  }

  SECTION("the waiting threads get it as well") {
    constexpr size_t CALLS = 200;
    std::atomic<size_t> thrown{0};
    sc::threading::parallelFor(CALLS, 1, [&](size_t first, size_t last) {
      for (size_t i = first; i < last; ++i) {
        try {
          // Every call either loads and throws or waits for one that does.
          (void) cache.load(1, []() -> Texture {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            throw std::runtime_error("no file");
          });
        } catch (const std::runtime_error &) {
          thrown.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
    CHECK(thrown.load() == CALLS);
    CHECK(cache.size() == 0);
    CHECK(cache.collect() >= 1);
    CHECK(Texture::alive.load() == 0);
  }
}

TEST_CASE("ECS Resource Owners", "[ecs][resource][threading]") {
  Texture::alive = 0;
  ResourceCache<Texture> cache(2);

  SECTION("collect gives back only the resources of its owner") {
    cache.load(1, 0, [] { return Texture(1); }).reset();
    cache.load(2, 1, [] { return Texture(2); }).reset();

    CHECK(cache.collect(1) == 1);
    CHECK(static_cast<bool>(cache.find(1)));
    CHECK(!cache.find(2));
    CHECK(Texture::alive.load() == 1);

    CHECK(cache.collect(0) == 1);
    CHECK(Texture::alive.load() == 0);
  }

  SECTION("owners collect at the same time") {
    constexpr int KEYS = 2000;
    std::atomic<bool> loading{true};
    std::atomic<size_t> collected{0};
    const auto collector = [&](uint8_t owner) {
      while (loading.load(std::memory_order_acquire)) {
        collected.fetch_add(cache.collect(owner), std::memory_order_relaxed);
      }
    };
    std::thread zero(collector, uint8_t{0});
    std::thread one(collector, uint8_t{1});
    for (int key = 0; key < KEYS; ++key) {
      // Load, drop, load again: what comes back may be the one waiting for
      // collect or a new one.
      const auto owner = static_cast<uint8_t>(key % 2);
      const auto id = static_cast<uint64_t>(key);
      cache.load(id, owner, [key] { return Texture(key); }).reset();
      const ResourceHandle<Texture> again = cache.load(id, owner, [key] { return Texture(key); });
      CHECK(again->id == key);
    }
    loading.store(false, std::memory_order_release);
    zero.join();
    one.join();
    collected.fetch_add(cache.collect(0) + cache.collect(1), std::memory_order_relaxed);

    CHECK(cache.size() == 0);
    CHECK(Texture::alive.load() == 0);
    CHECK(collected.load() >= KEYS);
  }
}

TEST_CASE("ECS Resource Handle As Component", "[ecs][resource]") {
  Texture::alive = 0;
  ResourceCache<Texture> cache;
  {
    World world;
    const auto entity = world.create(Material{cache.load(1, [] { return Texture(1); })});
    CHECK(world.get<Material>(entity).albedo->id == 1);
    CHECK(cache.collect() == 0);

    world.destroy(entity);
    CHECK(Texture::alive.load() == 1);
    CHECK(cache.collect() == 1);
    CHECK(Texture::alive.load() == 0);
  }
}

TEST_CASE("ECS Resource Load Async", "[ecs][resource][threading]") {
  sc::threading::init();
  Texture::alive = 0;

  SECTION("the handle comes before the resource") {
    ResourceCache<Texture> cache;
    std::atomic<bool> go{false};
    ResourceHandle<Texture> texture = cache.loadAsync(1, [&] {
      go.wait(false);
      return Texture(1);
    });
    REQUIRE(static_cast<bool>(texture));
    CHECK(!texture.ready());
    CHECK(!texture.failed());

    go.store(true);
    go.notify_all();
    texture.wait();
    CHECK(texture.ready());
    CHECK(texture->id == 1);
  }

  SECTION("many calls, one load") {
    ResourceCache<Texture> cache;
    std::atomic<int> loads{0};
    constexpr size_t CALLS = 1000;
    std::vector<ResourceHandle<Texture>> handles(CALLS);
    sc::threading::parallelFor(CALLS, 1, [&](size_t first, size_t last) {
      for (size_t i = first; i < last; ++i) {
        handles[i] = cache.loadAsync(7, [&] {
          loads.fetch_add(1, std::memory_order_relaxed);
          return Texture(7);
        });
      }
    });
    for (const ResourceHandle<Texture> &handle: handles) {
      handle.wait();
      REQUIRE(handle == handles[0]);
    }
    CHECK(loads.load() == 1);
  }

  SECTION("one who waits loads it while all workers are busy") {
    ResourceCache<Texture> cache;
    const size_t workers = sc::threading::threadSlots() - 1;
    std::atomic<size_t> busy{0};
    std::atomic<bool> release{false};
    for (size_t i = 0; i < workers; ++i) {
      sc::threading::detach([&] {
        busy.fetch_add(1);
        release.wait(false);
      });
    }
    while (busy.load() != workers) {
      std::this_thread::yield();
    }

    std::thread::id loadedOn;
    const ResourceHandle<Texture> texture = cache.loadAsync(1, [&] {
      loadedOn = std::this_thread::get_id();
      return Texture(1);
    });
    // Without a free worker the task waits in the queue: wait takes the load.
    texture.wait();
    CHECK(loadedOn == std::this_thread::get_id());
    CHECK(texture->id == 1);

    release.store(true);
    release.notify_all();
    sc::threading::waitUntilFinished();
  }

  SECTION("a loader that throws") {
    ResourceCache<Texture> cache;
    ResourceHandle<Texture> texture = cache.loadAsync(1, []() -> Texture { throw std::runtime_error("no file"); });
    CHECK_THROWS_AS(texture.wait(), std::runtime_error);
    CHECK(texture.failed());
    CHECK(!texture.ready());
    CHECK(cache.size() == 0);
    texture.reset();
    CHECK(cache.collect() == 1);
  }

  SECTION("the cache waits for its tasks") {
    std::atomic<int> loads{0};
    {
      ResourceCache<Texture> cache;
      cache
          .loadAsync(1,
                     [&] {
                       std::this_thread::sleep_for(std::chrono::milliseconds(20));
                       loads.fetch_add(1);
                       return Texture(1);
                     })
          .reset();
    }
    CHECK(loads.load() == 1);
  }
  CHECK(Texture::alive.load() == 0);
}
