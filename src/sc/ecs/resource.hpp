#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <ankerl/unordered_dense.h>

#include "sc/hash/hash.hpp"
#include "sc/threading/threading.hpp"

namespace sc::ecs {

  template<typename T>
  class ResourceHandle;

  // What a ResourceCache of T loads with: called without arguments, it gives
  // a T, or something to make one of. A T itself is made in place, so the
  // type need not be movable, which GPU objects often are not;
  // std::constructible_from would ask for a move.
  template<typename Loader, typename T>
  concept ResourceLoader = std::invocable<Loader &> && (std::same_as<std::invoke_result_t<Loader &>, T> ||
                                                        std::constructible_from<T, std::invoke_result_t<Loader &>>);

} // namespace sc::ecs

namespace sc::ecs::internal {

  enum class ResourceState : uint8_t { Loading, Ready, Failed };

  // What a ResourceHandle points at: the resource and its count. The cache
  // makes it with new and keeps it at the same address until collect.
  template<typename T>
  struct ResourceEntry {
    // The handles there are in the low bits. QUEUED says the entry is on the
    // unused list of its owner or with a collect that took it from there: it
    // is set by the handle that takes the count to 0 and cleared by collect
    // when the entry is in use again. One word for both, so that a handle
    // never touches the entry after its last change of it; collect may free
    // it right after.
    static constexpr uint32_t QUEUED = uint32_t{1} << 31;
    static constexpr uint32_t COUNT = QUEUED - 1;

    std::atomic<uint32_t> refs{1};
    std::atomic<ResourceState> state{ResourceState::Loading};
    // False while the load of loadAsync waits in the queue of the pool: the
    // first thread to set it calls pending, the task or one that waits for
    // the resource. True from the start for every other load.
    std::atomic<bool> claimed;
    uint8_t owner;
    // Out of the map, by forceLoad or a failed load. Under the lock of the cache.
    bool orphan = false;
    ResourceEntry *nextUnused = nullptr;
    std::atomic<ResourceEntry *> *unused;
    // The load loadAsync left for later: run(pending, true) loads, run(pending,
    // false) lets it fail without calling the loader. Either frees pending.
    void (*runPending)(void *pending, bool load) noexcept = nullptr;
    void *pending = nullptr;
    std::exception_ptr error;
    alignas(T) std::byte storage[sizeof(T)];

    ResourceEntry(uint8_t owner, std::atomic<ResourceEntry *> *unused, bool claimed) noexcept :
        claimed(claimed), owner(owner), unused(unused) {}

    [[nodiscard]] T &value() noexcept { return *std::launder(reinterpret_cast<T *>(storage)); }

    void acquire() noexcept {
      [[maybe_unused]] const uint32_t before = refs.fetch_add(1, std::memory_order_relaxed);
      assert((before & COUNT) != COUNT);
    }

    // The last handle puts the entry on the unused list of its owner, once.
    void release() noexcept {
      uint32_t before = refs.load(std::memory_order_relaxed);
      uint32_t after;
      do {
        assert((before & COUNT) != 0);
        after = before - 1;
        if (after == 0) {
          after = QUEUED;
        }
      } while (!refs.compare_exchange_weak(before, after, std::memory_order_acq_rel, std::memory_order_relaxed));
      if ((before & QUEUED) == 0 && after == QUEUED) {
        // A push only and collect takes the whole list, so no ABA.
        ResourceEntry *head = unused->load(std::memory_order_relaxed);
        do {
          nextUnused = head;
        } while (!unused->compare_exchange_weak(head, this, std::memory_order_release, std::memory_order_relaxed));
      }
    }

    // For collect, under the lock of the cache, so that no load can give it
    // a handle at the same time: true if nobody has it and it may go, else
    // the entry is in use again and its last handle queues it anew.
    [[nodiscard]] bool unusedNow() noexcept {
      uint32_t now = refs.load(std::memory_order_acquire);
      while (true) {
        assert((now & QUEUED) != 0);
        if ((now & COUNT) == 0) {
          return true;
        }
        // Release: collect has read nextUnused, which the handle that queues
        // the entry anew writes.
        if (refs.compare_exchange_weak(now, now & COUNT, std::memory_order_acq_rel, std::memory_order_acquire)) {
          return false;
        }
      }
    }

    // Takes the load loadAsync left in the queue of the pool, if nobody has
    // yet, and runs it (load) or lets it fail. True if this thread took it.
    bool claim(bool load) noexcept {
      if (claimed.load(std::memory_order_relaxed) || claimed.exchange(true, std::memory_order_acq_rel)) {
        return false;
      }
      runPending(pending, load);
      return true;
    }

    // Until the load of the entry is over. A load of loadAsync that has not
    // started yet is done here, so that threads of the pool that all wait for
    // it do not wait for a task none of them is free to run. Otherwise a
    // thread that waits joins a parallelFor of another thread meanwhile, the
    // loader's among them, and only sleeps if there is none. Throws what the
    // loader threw.
    void wait() {
      claim(true);
      while (state.load(std::memory_order_acquire) == ResourceState::Loading) {
        const auto loaded = [this] { return state.load(std::memory_order_acquire) != ResourceState::Loading; };
        if (!threading::helpThread(loaded)) {
          state.wait(ResourceState::Loading, std::memory_order_acquire);
        }
      }
      if (state.load(std::memory_order_relaxed) == ResourceState::Failed) {
        std::rethrow_exception(error);
      }
    }
  };

} // namespace sc::ecs::internal

namespace sc::ecs {

  template<typename T, typename Key, typename Hash>
  class ResourceCache;

  // A resource of a ResourceCache, or none. Small enough for a component:
  // one pointer, and getting at the resource is one load, no lookup.
  //
  //   struct Material { ResourceHandle<Texture> albedo; };
  //
  // A copy counts itself in with one atomic add, a move costs nothing. When
  // the last handle of a resource goes, nothing is freed yet: the resource
  // waits for collect of its owner, see ResourceCache.
  //
  // A handle of loadAsync may come before its resource: ready() says when it
  // is there, and only then may it be used.
  //
  // Handles may be copied and dropped on any thread. The resource itself is
  // shared: whoever writes it while others read keeps them apart.
  template<typename T>
  class ResourceHandle {
  public:
    ResourceHandle() noexcept = default;

    ResourceHandle(const ResourceHandle &other) noexcept : m_entry(other.m_entry) {
      if (m_entry != nullptr) {
        m_entry->acquire();
      }
    }

    ResourceHandle(ResourceHandle &&other) noexcept : m_entry(std::exchange(other.m_entry, nullptr)) {}

    ResourceHandle &operator=(const ResourceHandle &other) noexcept {
      ResourceHandle(other).swap(*this);
      return *this;
    }

    ResourceHandle &operator=(ResourceHandle &&other) noexcept {
      ResourceHandle(std::move(other)).swap(*this);
      return *this;
    }

    ~ResourceHandle() { reset(); }

    void reset() noexcept {
      if (m_entry != nullptr) {
        std::exchange(m_entry, nullptr)->release();
      }
    }

    void swap(ResourceHandle &other) noexcept { std::swap(m_entry, other.m_entry); }

    [[nodiscard]] T &operator*() const noexcept {
      assert(ready());
      return m_entry->value();
    }

    [[nodiscard]] T *operator->() const noexcept { return &**this; }

    // Whether it has a resource, loaded or not.
    [[nodiscard]] explicit operator bool() const noexcept { return m_entry != nullptr; }

    // Whether the resource is loaded and may be used. Not yet while loadAsync
    // loads it, never if its loader threw.
    [[nodiscard]] bool ready() const noexcept {
      return m_entry != nullptr && m_entry->state.load(std::memory_order_acquire) == internal::ResourceState::Ready;
    }

    // Whether its loader threw. The handle stays without a resource; a load
    // of the key loads it anew.
    [[nodiscard]] bool failed() const noexcept {
      return m_entry != nullptr && m_entry->state.load(std::memory_order_acquire) == internal::ResourceState::Failed;
    }

    // Until the resource is loaded, as load waits, and throws what its loader
    // threw. A load of loadAsync that has not started is done here.
    void wait() const {
      assert(m_entry != nullptr);
      m_entry->wait();
    }

    // The same resource, not just an equal one.
    [[nodiscard]] friend bool operator==(const ResourceHandle &, const ResourceHandle &) noexcept = default;

  private:
    template<typename, typename, typename>
    friend class ResourceCache;

    // Takes over a count the cache has given the entry already.
    explicit ResourceHandle(internal::ResourceEntry<T> *entry) noexcept : m_entry(entry) {}

    internal::ResourceEntry<T> *m_entry = nullptr;
  };

  // Resources shared by key, each loaded once, as entt's resource_cache, and
  // given back only where their owner says.
  //
  //   ResourceCache<Texture> textures;
  //   ResourceHandle<Texture> albedo = textures.load(assetId, [&] { return Texture(path); });
  //   ...
  //   scheduler.add([&textures] { textures.collect(); }, "collect textures");
  //
  // load is safe from any thread at any time. If several threads ask for a
  // key that is not there yet, one of them calls its loader and the others
  // wait for that resource: they join a parallelFor meanwhile, the loader's
  // if it has one, and sleep if there is none. The loader runs on the
  // calling thread and without a lock, so it may load other resources of the
  // same cache. A loader that waits for a task of the pool while all workers
  // wait for its resource never gets it, as with any other wait on the pool.
  //
  // In a frame, where waiting is a hitch, loadAsync instead: the loader runs
  // on the pool and the handle comes right away, ready() once it is loaded.
  //
  // A resource whose last handle went is given back only by collect, on the
  // thread that calls it, and not where the last component that had it was
  // destroyed. Until then load gives it out again without loading it anew.
  // Every resource has an owner, a number below the one given to the
  // constructor, which load gives it, and collect(owner) leaves the
  // resources of other owners alone: for resources that go at points of
  // their own.
  //
  // The cache knows no frames. What the GPU may still use has to wait for
  // the fence of its frame: the destructor of such a resource only puts it
  // into a memory::DeletionQueue, and collect is safe anywhere.
  //
  // The key is the asset id. With the default, a 64-bit hash such as
  // sc::hash of package path and object name, two of n assets have the same
  // one with a chance of about n^2 / 2^65, 3e-8 for a million. A key of the
  // full name rules that out.
  template<typename T, typename Key = h64, typename Hash = ankerl::unordered_dense::hash<Key>>
  class ResourceCache {
    using Base = internal::ResourceEntry<T>;

    // The key is needed again when collect takes the entry out of the map.
    struct Entry : Base {
      Key key;

      Entry(Key key, uint8_t owner, std::atomic<Base *> *unused, bool claimed = true) :
          Base(owner, unused, claimed), key(std::move(key)) {}
    };

  public:
    explicit ResourceCache(uint8_t owners = 1) : m_owners(owners), m_unused(std::make_unique<UnusedList[]>(owners)) {
      assert(owners != 0);
    }

    // Everything there is goes. Waits for the tasks of loadAsync first. No
    // handle may be left, and no load running.
    ~ResourceCache() {
      {
        std::unique_lock lock(m_mutex);
        m_tasksDone.wait(lock, [this] { return m_tasks == 0; });
      }
      // The orphans are only on the lists, the others in the map as well:
      // the lists first, before anything of the map is freed.
      for (uint8_t owner = 0; owner < m_owners; ++owner) {
        Base *entry = m_unused[owner].head.load(std::memory_order_acquire);
        while (entry != nullptr) {
          Base *const next = entry->nextUnused;
          if (entry->orphan) {
            destroy(entry);
          }
          entry = next;
        }
      }
      for (const auto &[key, entry]: m_map) {
        assert((entry->refs.load(std::memory_order_relaxed) & Base::COUNT) == 0);
        assert(entry->state.load(std::memory_order_relaxed) != internal::ResourceState::Loading);
        destroy(entry);
      }
    }

    ResourceCache(const ResourceCache &) = delete;
    ResourceCache &operator=(const ResourceCache &) = delete;

    // The resource of the key, loaded by loader() if it is not there, for
    // owner 0.
    template<typename Loader>
      requires ResourceLoader<Loader, T>
    ResourceHandle<T> load(const Key &key, Loader &&loader) {
      return load(key, 0, loader);
    }

    // The same for the given owner. The owner belongs to the resource: if it
    // is there already, it keeps the one it was loaded with, and debug builds
    // assert that it is the same.
    //
    // If loader throws, nothing is kept: this thread and every one that
    // waited for the resource get the exception, the next load calls a loader
    // again.
    template<typename Loader>
      requires ResourceLoader<Loader, T>
    ResourceHandle<T> load(const Key &key, uint8_t owner, Loader &&loader) {
      assert(owner < m_owners);
      Entry *entry;
      bool made = false;
      {
        std::scoped_lock lock(m_mutex);
        if (const auto found = m_map.find(key); found != m_map.end()) {
          entry = found->second;
          assert(entry->owner == owner);
          entry->acquire();
        } else {
          auto fresh = std::make_unique<Entry>(key, owner, &m_unused[owner].head);
          m_map.emplace(key, fresh.get());
          entry = fresh.release();
          made = true;
        }
      }
      if (made) {
        return fill(entry, loader);
      }
      ResourceHandle<T> handle(entry);
      entry->wait();
      return handle;
    }

    // The resource of the key without waiting for it, for owner 0.
    template<typename Loader>
      requires ResourceLoader<std::decay_t<Loader>, T> && std::constructible_from<std::decay_t<Loader>, Loader>
    ResourceHandle<T> loadAsync(const Key &key, Loader &&loader) {
      return loadAsync(key, 0, std::forward<Loader>(loader));
    }

    // The resource of the key without waiting for it: if it is not there, a
    // copy of loader is called by a task of the pool, and the handle comes
    // right away, ready() once it is loaded and failed() if loader threw. A
    // thread that waits for it (load, find, wait) before the task starts
    // calls the loader itself. Without workers in the pool it is called here.
    //
    // threading::shutdown drops tasks that have not started: their
    // resources fail.
    template<typename Loader>
      requires ResourceLoader<std::decay_t<Loader>, T> && std::constructible_from<std::decay_t<Loader>, Loader>
    ResourceHandle<T> loadAsync(const Key &key, uint8_t owner, Loader &&loader) {
      assert(owner < m_owners);
      using Job = PendingLoad<std::decay_t<Loader>>;
      Entry *entry;
      {
        std::scoped_lock lock(m_mutex);
        if (const auto found = m_map.find(key); found != m_map.end()) {
          entry = found->second;
          assert(entry->owner == owner);
          entry->acquire();
          return ResourceHandle<T>(entry);
        }
        auto fresh = std::make_unique<Entry>(key, owner, &m_unused[owner].head, false);
        auto job = std::make_unique<Job>(this, fresh.get(), std::forward<Loader>(loader));
        m_map.emplace(key, fresh.get());
        entry = fresh.release();
        entry->runPending = &Job::run;
        entry->pending = job.release();
        // One count for the caller, one for the task.
        entry->acquire();
        ++m_tasks;
      }
      ResourceHandle<T> handle(entry);
      Task task(this, entry);
      if (threading::threadSlots() <= 1) {
        task();
      } else {
        threading::detach(std::move(task));
      }
      return handle;
    }

    // Loads the resource of the key in any case, for owner 0.
    template<typename Loader>
      requires ResourceLoader<Loader, T>
    ResourceHandle<T> forceLoad(const Key &key, Loader &&loader) {
      return forceLoad(key, 0, loader);
    }

    // Loads the resource of the key in any case, and from now on load gives
    // out the new one. Handles of the old one keep it until they go; then it
    // waits for collect of its owner, as any other.
    template<typename Loader>
      requires ResourceLoader<Loader, T>
    ResourceHandle<T> forceLoad(const Key &key, uint8_t owner, Loader &&loader) {
      assert(owner < m_owners);
      Entry *entry;
      {
        std::scoped_lock lock(m_mutex);
        auto made = std::make_unique<Entry>(key, owner, &m_unused[owner].head);
        const auto [place, inserted] = m_map.try_emplace(key, made.get());
        if (!inserted) {
          place->second->orphan = true;
          place->second = made.get();
        }
        entry = made.release();
      }
      return fill(entry, loader);
    }

    // The resource of the key, or none if it is not there. One that is being
    // loaded is waited for, and what its loader throws is thrown here.
    [[nodiscard]] ResourceHandle<T> find(const Key &key) {
      Entry *entry;
      {
        std::scoped_lock lock(m_mutex);
        const auto found = m_map.find(key);
        if (found == m_map.end()) {
          return {};
        }
        entry = found->second;
        entry->acquire();
      }
      ResourceHandle<T> handle(entry);
      entry->wait();
      return handle;
    }

    // Gives back the resources of the owner that have had no handle since
    // their last one went, and returns how many. Their destructors run here,
    // on the calling thread, without the lock. Safe next to everything else
    // and next to collect of other owners; resources of other owners stay.
    //
    // No handle means no component has it, not that the GPU is done with it:
    // the destructor of a GPU resource puts it into a memory::DeletionQueue,
    // which frees it once the fence of its frame has signalled.
    size_t collect(uint8_t owner = 0) {
      assert(owner < m_owners);
      Base *list = m_unused[owner].head.exchange(nullptr, std::memory_order_acquire);
      if (list == nullptr) {
        return 0;
      }
      Base *dead = nullptr;
      {
        std::scoped_lock lock(m_mutex);
        while (list != nullptr) {
          Base *const entry = std::exchange(list, list->nextUnused);
          if (entry->unusedNow()) {
            if (!entry->orphan) {
              m_map.erase(static_cast<Entry *>(entry)->key);
            }
            entry->nextUnused = dead;
            dead = entry;
          }
        }
      }
      size_t count = 0;
      while (dead != nullptr) {
        destroy(std::exchange(dead, dead->nextUnused));
        ++count;
      }
      return count;
    }

    // How many keys have a resource, loaded or being loaded, also ones that
    // wait for collect.
    [[nodiscard]] size_t size() const {
      std::scoped_lock lock(m_mutex);
      return m_map.size();
    }

    [[nodiscard]] uint8_t owners() const noexcept { return m_owners; }

  private:
    struct alignas(64) UnusedList {
      std::atomic<Base *> head{nullptr};
    };

    // The load loadAsync leaves for later, with a copy of the loader. The
    // thread that claims the entry calls run once, which frees it.
    template<typename Loader>
    struct PendingLoad {
      ResourceCache *cache;
      Entry *entry;
      Loader loader;

      template<typename From>
      PendingLoad(ResourceCache *cache, Entry *entry, From &&loader) :
          cache(cache), entry(entry), loader(std::forward<From>(loader)) {}

      // noexcept on purpose: if fail throws (no memory for the error, a mutex
      // that fails), the entry would stay loading and whoever waits for it
      // would wait for ever; the program ends instead.
      // NOLINTNEXTLINE(bugprone-exception-escape): see above
      static void run(void *pending, bool load) noexcept {
        const std::unique_ptr<PendingLoad> job(static_cast<PendingLoad *>(pending));
        if (load) {
          job->entry->acquire();
          try {
            (void) job->cache->fill(job->entry, job->loader);
            // NOLINTNEXTLINE(bugprone-empty-catch): fail kept it in the entry for whoever waits or looks
          } catch (...) {
          }
        } else {
          job->cache->fail(job->entry, std::make_exception_ptr(std::runtime_error(
                                           "ResourceCache: the task of loadAsync was dropped before it ran")));
        }
      }
    };

    // The task of loadAsync on the pool. Holds one count of the entry and is
    // counted in m_tasks until it goes, whether it ran or was dropped.
    class Task {
    public:
      Task(ResourceCache *cache, Base *entry) noexcept : m_cache(cache), m_entry(entry) {}
      Task(Task &&other) noexcept : m_cache(other.m_cache), m_entry(std::exchange(other.m_entry, nullptr)) {}
      Task &operator=(Task &&) = delete;

      ~Task() {
        if (m_entry != nullptr) {
          // Nothing if it ran or somebody who waited loaded it.
          m_entry->claim(false);
          m_entry->release();
          m_cache->taskDone();
        }
      }

      void operator()() noexcept { m_entry->claim(true); }

    private:
      ResourceCache *m_cache;
      Base *m_entry;
    };

    void taskDone() noexcept {
      std::scoped_lock lock(m_mutex);
      if (--m_tasks == 0) {
        m_tasksDone.notify_all();
      }
    }

    // Calls the loader for an entry that is loading, and takes over one count
    // of it for the handle it returns.
    template<typename Loader>
    ResourceHandle<T> fill(Entry *entry, Loader &loader) {
      ResourceHandle<T> handle(entry);
      try {
        ::new (static_cast<void *>(entry->storage)) T(loader());
      } catch (...) {
        fail(entry, std::current_exception());
        throw;
      }
      entry->state.store(internal::ResourceState::Ready, std::memory_order_release);
      entry->state.notify_all();
      return handle;
    }

    // Nothing is kept of a load that failed: out of the map, so that the
    // next load loads anew.
    void fail(Entry *entry, std::exception_ptr error) {
      entry->error = std::move(error);
      {
        std::scoped_lock lock(m_mutex);
        if (const auto found = m_map.find(entry->key); found != m_map.end() && found->second == entry) {
          m_map.erase(found);
        }
        entry->orphan = true;
      }
      entry->state.store(internal::ResourceState::Failed, std::memory_order_release);
      entry->state.notify_all();
    }

    static void destroy(Base *entry) noexcept {
      if (entry->state.load(std::memory_order_relaxed) == internal::ResourceState::Ready) {
        entry->value().~T();
      }
      delete static_cast<Entry *>(entry);
    }

    uint8_t m_owners;
    std::unique_ptr<UnusedList[]> m_unused;
    mutable std::mutex m_mutex;
    // TODO: one mutex for every load, also the ones that find their resource.
    // Not measured how much threads that load many small resources at once
    // wait for it; if they do, a map split by hash with a mutex each.
    ankerl::unordered_dense::map<Key, Entry *, Hash> m_map;
    // Tasks of loadAsync that have not gone yet, under m_mutex.
    size_t m_tasks = 0;
    std::condition_variable m_tasksDone;
  };

} // namespace sc::ecs
