#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
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

  // Files under a directory of their own in the temp directory, removed at
  // the end. A file's first byte is the id of its texture.
  struct TextureFiles {
    std::filesystem::path dir;

    TextureFiles() :
        dir(std::filesystem::temp_directory_path() /
            ("sc_resource_" + std::to_string(reinterpret_cast<uintptr_t>(this)))) {
      std::filesystem::create_directories(dir);
    }

    ~TextureFiles() {
      std::error_code ignored;
      std::filesystem::remove_all(dir, ignored);
    }

    TextureFiles(const TextureFiles &) = delete;
    TextureFiles &operator=(const TextureFiles &) = delete;

    [[nodiscard]] std::string path(int id) const { return (dir / (std::to_string(id) + ".tex")).string(); }

    // NOLINTNEXTLINE(modernize-use-nodiscard): written for the file, the path is a convenience
    std::string write(int id) const {
      const std::string file = path(id);
      std::ofstream stream(file, std::ios::binary);
      stream.put(static_cast<char>(id));
      stream << std::string(1000, 'x');
      return file;
    }

    [[nodiscard]] std::string missing() const { return (dir / "missing.tex").string(); }
  };

  // Called on the lane, where Catch's macros must not be used: a file of
  // the wrong size gives -1, which the CHECKs of the id see.
  Texture fromBytes(std::span<const std::byte> bytes) {
    return Texture(bytes.size() == 1001 ? static_cast<int>(bytes[0]) : -1);
  }


  // Keeps every thread of the lane busy until release, so that a task of
  // bind waits in its queue.
  std::vector<sc::threading::BackgroundTask<void>> blockLane(std::atomic<bool> &release) {
    const size_t threads = sc::threading::backgroundThreadCount();
    std::atomic<size_t> busy{0};
    std::vector<sc::threading::BackgroundTask<void>> blockers;
    for (size_t i = 0; i < threads; ++i) {
      blockers.push_back(sc::threading::spawn([&busy, &release] {
        busy.fetch_add(1);
        release.wait(false);
      }));
    }
    while (busy.load() != threads) {
      std::this_thread::yield();
    }
    return blockers;
  }

  void unblock(std::atomic<bool> &release, std::vector<sc::threading::BackgroundTask<void>> &blockers) {
    release.store(true);
    release.notify_all();
    for (auto &blocker: blockers) {
      blocker.get();
    }
  }
} // namespace

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

  SECTION("a bind before collect gets it back without loading") {
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

  SECTION("a bind after collect loads it anew") {
    cache.load(1, loader).reset();
    CHECK(cache.collect() == 1);
    const ResourceHandle<Texture> again = cache.load(1, loader);
    CHECK(loads == 2);
    CHECK(again->id == 1);
  }

  SECTION("find gives what is there, before apply and after") {
    CHECK(!cache.find(1));
    const ResourceHandle<Texture> loaded = cache.load(1, loader);
    CHECK(cache.find(1) == loaded);
    CHECK(cache.apply() == 1);
    CHECK(cache.find(1) == loaded);
    CHECK(cache.apply() == 0);
  }

  SECTION("get lends what apply took in and counts nothing") {
    ResourceHandle<Texture> loaded = cache.load(1, loader);
    CHECK(cache.get(1) == nullptr);
    cache.apply();
    const Texture *const lent = cache.get(1);
    REQUIRE(lent == &*loaded);
    loaded.reset();
    // The pointer kept nothing: the last handle went, so collect gives it back.
    CHECK(cache.collect() == 1);
    CHECK(cache.get(1) == nullptr);
  }
}

TEST_CASE("ECS Resource Force Load", "[ecs][resource]") {
  Texture::alive = 0;
  ResourceCache<Texture> cache;

  ResourceHandle<Texture> old = cache.load(1, [] { return Texture(1); });

  SECTION("in place: every handle sees the new one") {
    const ResourceHandle<Texture> fresh = cache.forceLoad(1, [] { return Texture(2); });
    CHECK(fresh == old);
    CHECK(old->id == 2);
    CHECK(Texture::alive.load() == 1);
    CHECK(cache.size() == 1);
  }

  SECTION("a key that is not there") {
    const ResourceHandle<Texture> other = cache.forceLoad(2, [] { return Texture(2); });
    CHECK(other->id == 2);
    CHECK(cache.size() == 2);
  }

  SECTION("a loader that throws leaves it failed") {
    CHECK_THROWS_AS(cache.forceLoad(1, []() -> Texture { throw std::runtime_error("no file"); }), std::runtime_error);
    CHECK(old.failed());
    CHECK(Texture::alive.load() == 0);
  }

  SECTION("its loader may bind other keys") {
    ResourceHandle<Texture> inner;
    (void) cache.forceLoad(1, [&] {
      inner = cache.load(2, [] { return Texture(2); });
      return Texture(3);
    });
    CHECK(old->id == 3);
    CHECK(inner->id == 2);
  }
}

TEST_CASE("ECS Resource Loader Throws", "[ecs][resource][threading]") {
  sc::threading::init();
  Texture::alive = 0;
  ResourceCache<Texture> cache;
  int loads = 0;
  const auto throwing = [&]() -> Texture {
    ++loads;
    throw std::runtime_error("no file");
  };

  SECTION("the failed one stays until nobody has it") {
    CHECK_THROWS_AS(cache.load(1, throwing), std::runtime_error);
    CHECK(cache.size() == 1);
    ResourceHandle<Texture> failed = cache.find(1);
    REQUIRE(static_cast<bool>(failed));
    CHECK(failed.failed());

    // Binding the key again throws without calling a loader.
    CHECK_THROWS_AS(cache.load(1, [] { return Texture(1); }), std::runtime_error);
    CHECK(loads == 1);

    failed.reset();
    // The failed entry is given back without a destructor of a texture.
    CHECK(cache.collect() == 1);
    const ResourceHandle<Texture> texture = cache.load(1, [] { return Texture(1); });
    CHECK(texture->id == 1);
  }

  SECTION("forceLoad loads it anew") {
    CHECK_THROWS_AS(cache.load(1, throwing), std::runtime_error);
    const ResourceHandle<Texture> texture = cache.forceLoad(1, [] { return Texture(1); });
    CHECK(texture->id == 1);
  }

  SECTION("whoever waits for it gets it") {
    ResourceHandle<Texture> texture = cache.bind(1, []() -> Texture {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      throw std::runtime_error("no file");
    });
    constexpr size_t WAITERS = 200;
    std::atomic<size_t> thrown{0};
    sc::threading::parallelFor(WAITERS, 1, [&](size_t first, size_t last) {
      for (size_t i = first; i < last; ++i) {
        // NOLINTNEXTLINE(performance-unnecessary-copy-initialization): copies on many threads at once
        const ResourceHandle<Texture> copy = texture;
        try {
          copy.wait();
        } catch (const std::runtime_error &) {
          thrown.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
    CHECK(thrown.load() == WAITERS);
    CHECK(cache.size() == 1);
    texture.reset();
    sc::threading::waitUntilBackgroundFinished();
    CHECK(cache.collect() == 1);
  }
}

TEST_CASE("ECS Resource Owners", "[ecs][resource][threading]") {
  sc::threading::init();
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

  SECTION("collect next to tasks that let go of their resources") {
    constexpr int KEYS = 2000;
    for (int key = 0; key < KEYS; ++key) {
      // The task holds the only count once the handle is gone: it queues the
      // entry on its thread, maybe while collect below takes the list.
      const auto owner = static_cast<uint8_t>(key % 2);
      cache.bind(static_cast<uint64_t>(key), owner, [key] { return Texture(key); }).reset();
      (void) cache.collect(owner);
    }
    sc::threading::waitUntilBackgroundFinished();
    (void) cache.collect(0);
    (void) cache.collect(1);

    CHECK(cache.size() == 0);
    CHECK(Texture::alive.load() == 0);
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

TEST_CASE("ECS Resource Bind", "[ecs][resource][threading]") {
  sc::threading::init();
  Texture::alive = 0;

  SECTION("with the loader of the cache") {
    std::atomic<int> loads{0};
    ResourceCache<Texture> cache([&](const sc::h64 &key) {
      loads.fetch_add(1);
      return Texture(static_cast<int>(key));
    });
    const ResourceHandle<Texture> texture = cache.bind(5);
    REQUIRE(static_cast<bool>(texture));
    texture.wait();
    CHECK(texture->id == 5);
    CHECK(cache.bind(5) == texture);
    CHECK(loads.load() == 1);
  }

  SECTION("the loader runs on the lane, not on the pool") {
    ResourceCache<Texture> cache;
    std::atomic<bool> onLane{false};
    const ResourceHandle<Texture> texture = cache.bind(1, [&] {
      onLane = sc::threading::onBackgroundThread();
      return Texture(1);
    });
    sc::threading::waitUntilBackgroundFinished();
    CHECK(texture.ready());
    CHECK(onLane.load());
  }

  SECTION("without a loader of the cache") {
    const ResourceCache<Texture> cache;
    CHECK_THROWS_AS((void) cache.bind(5), std::logic_error);
  }

  SECTION("the handle comes before the resource") {
    ResourceCache<Texture> cache;
    std::atomic<bool> go{false};
    ResourceHandle<Texture> texture = cache.bind(1, [&] {
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

  SECTION("many threads, one key, one load") {
    ResourceCache<Texture> cache;
    std::atomic<int> loads{0};
    constexpr size_t HANDLES = 10000;
    std::vector<ResourceHandle<Texture>> handles(HANDLES);
    sc::threading::parallelFor(HANDLES, 64, [&](size_t first, size_t last) {
      for (size_t i = first; i < last; ++i) {
        handles[i] = cache.bind(7, [&] {
          loads.fetch_add(1, std::memory_order_relaxed);
          return Texture(7);
        });
      }
    });
    for (const ResourceHandle<Texture> &handle: handles) {
      REQUIRE(handle == handles[0]);
    }
    handles[0].wait();
    CHECK(loads.load() == 1);
    CHECK(cache.size() == 1);
  }

  SECTION("many threads, many keys, one load each") {
    ResourceCache<Texture> cache;
    std::atomic<int> loads{0};
    constexpr size_t HANDLES = 10000;
    constexpr size_t KEYS = 500;
    std::vector<ResourceHandle<Texture>> handles(HANDLES);
    sc::threading::parallelFor(HANDLES, 16, [&](size_t first, size_t last) {
      for (size_t i = first; i < last; ++i) {
        const auto key = static_cast<int>(i % KEYS);
        handles[i] = cache.bind(static_cast<uint64_t>(key), [&, key] {
          loads.fetch_add(1, std::memory_order_relaxed);
          return Texture(key);
        });
      }
    });
    for (size_t i = 0; i < HANDLES; ++i) {
      REQUIRE(handles[i] == handles[i % KEYS]);
    }
    sc::threading::waitUntilBackgroundFinished();
    CHECK(loads.load() == static_cast<int>(KEYS));
    CHECK(cache.size() == KEYS);
    CHECK(cache.apply() == KEYS);
    CHECK(cache.size() == KEYS);
  }

  SECTION("many threads bind and drop while tasks park and start again") {
    // Measured with counters in the entry, while the tasks ran on the pool:
    // about 3 000 loads parked, 2 600 started again by a bind and 950 done
    // by a thread that waits. Four helpers bind; a key
    // is waited for by every third key only, and collect is rare enough for
    // a parked one to be bound again first.
    ResourceCache<Texture> cache;
    constexpr size_t BINDS = 20000;
    constexpr size_t KEYS = 2000;
    std::atomic<size_t> wrong{0};
    sc::threading::parallelFor(BINDS, 8, 4, [&](size_t first, size_t last) {
      for (size_t i = first; i < last; ++i) {
        const auto key = static_cast<int>(i % KEYS);
        const ResourceHandle<Texture> texture = cache.bind(static_cast<uint64_t>(key), [key] { return Texture(key); });
        if (key % 3 == 0) {
          texture.wait();
          if (texture->id != key) {
            wrong.fetch_add(1, std::memory_order_relaxed);
          }
        }
        if (i % 7919 == 0) {
          (void) cache.collect();
        }
      }
    });
    CHECK(wrong.load() == 0);
    sc::threading::waitUntilBackgroundFinished();
    (void) cache.collect();
    CHECK(cache.size() == 0);
  }

  SECTION("a loader binds what it needs") {
    ResourceCache<Texture> cache;
    ResourceHandle<Texture> inner;
    const ResourceHandle<Texture> outer = cache.bind(1, [&] {
      inner = cache.bind(2, [] { return Texture(2); });
      return Texture(1);
    });
    outer.wait();
    inner.wait();
    CHECK(inner->id == 2);
  }

  SECTION("a loader binds without a lane, where it runs in bind") {
    // A bind that held its lock while it ran the loader would wait for itself
    // if both keys fall into one shard; with 64 shards, one of 64 keys does.
    // init does nothing while the pool and the lane run.
    sc::threading::shutdown();
    sc::threading::init(0, 0);
    REQUIRE(sc::threading::backgroundThreadCount() == 0);
    {
      ResourceCache<Texture> cache;
      std::vector<ResourceHandle<Texture>> inner(64);
      const ResourceHandle<Texture> outer = cache.bind(0, [&] {
        for (size_t key = 0; key < inner.size(); ++key) {
          inner[key] = cache.bind(key + 1, [key] { return Texture(static_cast<int>(key)); });
        }
        return Texture(-1);
      });
      CHECK(outer.ready());
      CHECK(inner.back().ready());
    }
    sc::threading::shutdown();
    sc::threading::init();
  }

  SECTION("one who waits loads it while the lane is busy") {
    ResourceCache<Texture> cache;
    std::atomic<bool> release{false};
    std::vector<sc::threading::BackgroundTask<void>> blockers = blockLane(release);

    std::thread::id loadedOn;
    const ResourceHandle<Texture> texture = cache.bind(1, [&] {
      loadedOn = std::this_thread::get_id();
      return Texture(1);
    });
    // Without a free thread of the lane the task waits in its queue: wait
    // takes the load.
    texture.wait();
    CHECK(loadedOn == std::this_thread::get_id());
    CHECK(texture->id == 1);

    unblock(release, blockers);
    sc::threading::waitUntilBackgroundFinished();
  }

  SECTION("a loader that throws") {
    ResourceCache<Texture> cache;
    ResourceHandle<Texture> texture = cache.bind(1, []() -> Texture { throw std::runtime_error("no file"); });
    CHECK_THROWS_AS(texture.wait(), std::runtime_error);
    CHECK(texture.failed());
    CHECK(!texture.ready());
    CHECK(cache.size() == 1);
    texture.reset();
    // wait() returns once the state is Failed, but the task may still hold its
    // count then; the entry is queued by whoever lets go last.
    sc::threading::waitUntilBackgroundFinished();
    CHECK(cache.collect() == 1);
  }

  SECTION("the cache waits for its tasks") {
    std::atomic<int> loads{0};
    {
      ResourceCache<Texture> cache;
      std::atomic<bool> started{false};
      ResourceHandle<Texture> texture = cache.bind(1, [&] {
        started.store(true);
        started.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        loads.fetch_add(1);
        return Texture(1);
      });
      // Dropped before the task starts, the load would be parked.
      started.wait(false);
      texture.reset();
    }
    CHECK(loads.load() == 1);
  }

  SECTION("a load whose handles went before its task started waits for the next bind") {
    ResourceCache<Texture> cache;
    std::atomic<bool> release{false};
    std::vector<sc::threading::BackgroundTask<void>> blockers = blockLane(release);
    std::atomic<int> loads{0};
    const auto loader = [&] {
      loads.fetch_add(1);
      return Texture(1);
    };
    cache.bind(1, loader).reset();
    cache.bind(2, loader).reset();
    unblock(release, blockers);
    sc::threading::waitUntilBackgroundFinished();
    CHECK(loads.load() == 0);
    CHECK(cache.size() == 2);
    CHECK(cache.apply() == 2);

    SECTION("bind starts it again") {
      const ResourceHandle<Texture> texture = cache.bind(1, loader);
      texture.wait();
      CHECK(texture->id == 1);
      CHECK(loads.load() == 1);
    }

    SECTION("find starts it again") {
      const ResourceHandle<Texture> texture = cache.find(1);
      REQUIRE(static_cast<bool>(texture));
      texture.wait();
      CHECK(texture->id == 1);
      CHECK(loads.load() == 1);
    }

    SECTION("load loads it here") {
      const ResourceHandle<Texture> texture = cache.load(1, loader);
      CHECK(texture->id == 1);
      CHECK(loads.load() == 1);
    }

    SECTION("forceLoad loads only its own") {
      const ResourceHandle<Texture> texture = cache.forceLoad(1, [] { return Texture(3); });
      CHECK(texture->id == 3);
      CHECK(loads.load() == 0);
    }

    SECTION("collect gives it back without loading it") {
      CHECK(cache.collect() == 2);
      CHECK(cache.size() == 0);
    }

    sc::threading::waitUntilBackgroundFinished();
    CHECK(loads.load() <= 1);
  }

  SECTION("a task that shutdownBackground drops fails its resource, also one that waits in the cache") {
    ResourceCache<Texture> cache(1, 1);
    // Until shutdownBackground stops them: a thread of the lane that returns
    // from here stops then instead of taking the task of bind.
    const size_t threads = sc::threading::backgroundThreadCount();
    std::atomic<size_t> busy{0};
    std::vector<sc::threading::BackgroundTask<void>> blockers;
    for (size_t i = 0; i < threads; ++i) {
      blockers.push_back(sc::threading::spawn([&](const std::stop_token &stop) {
        busy.fetch_add(1);
        while (!stop.stop_requested()) {
          std::this_thread::yield();
        }
      }));
    }
    while (busy.load() != threads) {
      std::this_thread::yield();
    }

    std::atomic<int> loads{0};
    const auto loader = [&] {
      loads.fetch_add(1);
      return Texture(1);
    };
    ResourceHandle<Texture> texture = cache.bind(1, loader);
    // With one load at a time, this one waits in the cache.
    ResourceHandle<Texture> waiting = cache.bind(2, loader);
    sc::threading::shutdownBackground();
    for (auto &blocker: blockers) {
      blocker.get();
    }

    CHECK(texture.failed());
    CHECK(!texture.ready());
    CHECK_THROWS_AS(texture.wait(), std::runtime_error);
    CHECK(waiting.failed());
    CHECK(loads.load() == 0);
    CHECK(cache.size() == 2);
    texture.reset();
    waiting.reset();
    CHECK(cache.collect() == 2);
    // The tests after this one expect a lane.
    sc::threading::initBackground();
  }
  CHECK(Texture::alive.load() == 0);
}

TEST_CASE("ECS Resource Loads In Flight", "[ecs][resource][threading]") {
  sc::threading::init();
  Texture::alive = 0;
  const size_t threads = sc::threading::backgroundThreadCount();
  if (threads < 2) {
    SKIP("Needs two threads of the lane for loads to run at once");
  }

  // Binds 40 keys whose loaders take a while and returns how many of them
  // ran at once at most.
  const auto mostAtOnce = [](ResourceCache<Texture> &cache) {
    std::atomic<size_t> running{0};
    std::atomic<size_t> most{0};
    std::vector<ResourceHandle<Texture>> handles;
    for (int key = 0; key < 40; ++key) {
      handles.push_back(cache.bind(static_cast<uint64_t>(key), [&, key] {
        const size_t now = running.fetch_add(1) + 1;
        size_t seen = most.load();
        while (seen < now && !most.compare_exchange_weak(seen, now)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        running.fetch_sub(1);
        return Texture(key);
      }));
    }
    sc::threading::waitUntilBackgroundFinished();
    for (int key = 0; key < 40; ++key) {
      CHECK(handles[static_cast<size_t>(key)]->id == key);
    }
    return most.load();
  };

  SECTION("at most maxLoads at once") {
    ResourceCache<Texture> cache(1, 2);
    const size_t most = mostAtOnce(cache);
    CHECK(most >= 1);
    CHECK(most <= 2);
  }

  SECTION("half the lane by default") {
    ResourceCache<Texture> cache;
    const size_t most = mostAtOnce(cache);
    CHECK(most >= 1);
    CHECK(most <= std::max<size_t>(1, threads / 2));
  }

  SECTION("one who waits loads it while it waits in the cache") {
    ResourceCache<Texture> cache(1, 1);
    std::atomic<bool> release{false};
    const ResourceHandle<Texture> first = cache.bind(1, [&] {
      release.wait(false);
      return Texture(1);
    });
    std::thread::id loadedOn;
    const ResourceHandle<Texture> second = cache.bind(2, [&] {
      loadedOn = std::this_thread::get_id();
      return Texture(2);
    });
    second.wait();
    CHECK(loadedOn == std::this_thread::get_id());
    CHECK(!first.ready());

    release.store(true);
    release.notify_all();
    first.wait();
    sc::threading::waitUntilBackgroundFinished();
  }
  CHECK(Texture::alive.load() == 0);
}

TEST_CASE("ECS Resource Bind File", "[ecs][resource][io]") {
  sc::threading::init();
  Texture::alive = 0;
  const TextureFiles textures;
  std::atomic<int> builds{0};
  const auto build = [&](std::span<const std::byte> bytes) {
    builds.fetch_add(1);
    return fromBytes(bytes);
  };

  SECTION("the handle comes first, the bytes with apply, as the scheduler calls it after every stage") {
    ResourceCache<Texture> cache;
    const ResourceHandle<Texture> texture = cache.bind(9, textures.write(9), build);
    REQUIRE(static_cast<bool>(texture));
    // The bytes come only with a poll, which apply does.
    CHECK(!texture.ready());
    CHECK(cache.reads() == 1);
    int applies = 0;
    while (!texture.ready() && applies < 100000) {
      (void) cache.apply();
      ++applies;
      std::this_thread::yield();
    }
    REQUIRE(texture.ready());
    CHECK(texture->id == 9);
    CHECK(builds.load() == 1);
    CHECK(cache.reads() == 0);
  }

  SECTION("with the path and the build of the cache") {
    std::atomic<int> paths{0};
    ResourceCache<Texture> cache(
        [&](const sc::h64 &key) {
          paths.fetch_add(1);
          return textures.path(static_cast<int>(key));
        },
        build);
    textures.write(10);
    const ResourceHandle<Texture> first = cache.bind(10);
    const ResourceHandle<Texture> second = cache.bind(10);
    CHECK(first == second);
    first.wait();
    CHECK(first->id == 10);
    (void) cache.apply();
    // From the map apply fills, without a path either.
    CHECK(cache.bind(10) == first);
    // Only for the new key.
    CHECK(paths.load() == 1);

    ResourceHandle<Texture> missing = cache.bind(11);
    CHECK_THROWS_AS(missing.wait(), std::system_error);
    missing.reset();
    sc::threading::waitUntilBackgroundFinished();
    CHECK(cache.collect() == 1);
  }

  SECTION("a path that throws fails the resource") {
    ResourceCache<Texture> cache([](const sc::h64 &) -> std::string { throw std::runtime_error("no such asset"); },
                                 build);
    const ResourceHandle<Texture> texture = cache.bind(12);
    CHECK(texture.failed());
    CHECK_THROWS_AS(texture.wait(), std::runtime_error);
    CHECK(cache.reads() == 0);
  }

  SECTION("wait polls the reader itself") {
    ResourceCache<Texture> cache;
    const ResourceHandle<Texture> texture = cache.bind(2, textures.write(2), build);
    texture.wait();
    CHECK(texture->id == 2);
  }

  SECTION("a key bound again is read once") {
    ResourceCache<Texture> cache;
    const std::string path = textures.write(3);
    const ResourceHandle<Texture> first = cache.bind(3, path, build);
    const ResourceHandle<Texture> second = cache.bind(3, path, build);
    CHECK(first == second);
    second.wait();
    sc::threading::waitUntilBackgroundFinished();
    CHECK(builds.load() == 1);
    CHECK(cache.reads() == 0);
  }

  SECTION("a file that cannot be read fails the resource") {
    ResourceCache<Texture> cache;
    ResourceHandle<Texture> texture = cache.bind(4, textures.missing(), build);
    try {
      texture.wait();
      FAIL("no exception");
    } catch (const std::system_error &error) {
      CHECK(error.code() == std::errc::no_such_file_or_directory);
    }
    CHECK(texture.failed());
    CHECK(builds.load() == 0);
    texture.reset();
    CHECK(cache.collect() == 1);
  }

  SECTION("a build that throws fails the resource") {
    ResourceCache<Texture> cache;
    const ResourceHandle<Texture> texture = cache.bind(
        5, textures.write(5), [](std::span<const std::byte>) -> Texture { throw std::runtime_error("bad texture"); });
    CHECK_THROWS_AS(texture.wait(), std::runtime_error);
    CHECK(texture.failed());
  }

  SECTION("a read whose handles went is parked once its bytes are there") {
    ResourceCache<Texture> cache;
    const std::string path = textures.write(6);
    cache.bind(6, path, build).reset();
    while (cache.reads() != 0) {
      (void) cache.apply();
    }
    sc::threading::waitUntilBackgroundFinished();
    CHECK(builds.load() == 0);

    // Bound again, it is built from the bytes it has, without a read.
    const ResourceHandle<Texture> texture = cache.bind(6, path, build);
    CHECK(cache.reads() == 0);
    texture.wait();
    CHECK(texture->id == 6);
    CHECK(builds.load() == 1);
  }

  SECTION("the destructor of the cache polls for reads under way") {
    {
      ResourceCache<Texture> cache;
      cache.bind(7, textures.write(7), build).reset();
    }
    // Got here: the destructor has not waited for a poll nobody does.
    SUCCEED();
  }

  SECTION("many threads bind files, the waits poll") {
    ResourceCache<Texture> cache;
    constexpr int KEYS = 100;
    std::vector<std::string> paths;
    for (int key = 0; key < KEYS; ++key) {
      paths.push_back(textures.write(key));
    }
    std::vector<ResourceHandle<Texture>> handles(1000);
    sc::threading::parallelFor(handles.size(), 8, 4, [&](size_t first, size_t last) {
      for (size_t i = first; i < last; ++i) {
        const auto key = static_cast<int>(i % KEYS);
        handles[i] = cache.bind(static_cast<uint64_t>(key), paths[static_cast<size_t>(key)], build);
      }
    });
    for (size_t i = 0; i < handles.size(); ++i) {
      handles[i].wait();
      CHECK(handles[i]->id == static_cast<int>(i % KEYS));
    }
    sc::threading::waitUntilBackgroundFinished();
    CHECK(builds.load() == KEYS);
  }

  SECTION("without a lane the texture is built in the poll") {
    sc::threading::shutdown();
    sc::threading::init(0, 0);
    {
      ResourceCache<Texture> cache;
      const ResourceHandle<Texture> texture = cache.bind(8, textures.write(8), build);
      while (cache.reads() != 0) {
        (void) cache.apply();
      }
      CHECK(texture.ready());
      CHECK(texture->id == 8);
    }
    sc::threading::shutdown();
    sc::threading::init();
  }
  CHECK(Texture::alive.load() == 0);
}
