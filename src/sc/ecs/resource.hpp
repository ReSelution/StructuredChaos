#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
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

  // What runPending of an entry does with the load left for later.
  enum class PendingRun : uint8_t { Load, Fail, Free };

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
    //
    // PARKED says its task found nobody but itself holding the entry and left
    // the load in it instead of calling the loader: whoever finds the entry
    // by key next takes the bit and starts the load again. In the same word
    // as the count, so that no find can count itself in between the task
    // seeing 1 and setting it.
    static constexpr uint32_t QUEUED = uint32_t{1} << 31;
    static constexpr uint32_t PARKED = uint32_t{1} << 30;
    static constexpr uint32_t COUNT = PARKED - 1;

    std::atomic<uint32_t> refs{1};
    std::atomic<ResourceState> state{ResourceState::Loading};
    // False while its load waits in the queue of the pool: the first thread
    // to set it calls pending, the task or one that waits for the resource.
    // Also true while it is parked; the thread that takes PARKED clears it.
    std::atomic<bool> claimed{false};
    uint8_t owner;
    ResourceEntry *nextUnused = nullptr;
    std::atomic<ResourceEntry *> *unused;
    // The load left for later: Load loads, Fail lets it fail without calling
    // the loader, Free only frees it, for a parked entry that goes. Each frees
    // pending.
    void (*runPending)(void *pending, PendingRun run) noexcept = nullptr;
    void *pending = nullptr;
    std::exception_ptr error;
    alignas(T) std::byte storage[sizeof(T)];

    ResourceEntry(uint8_t owner, std::atomic<ResourceEntry *> *unused) noexcept : owner(owner), unused(unused) {}

    [[nodiscard]] T &value() noexcept { return *std::launder(reinterpret_cast<T *>(storage)); }

    void acquire() noexcept {
      [[maybe_unused]] const uint32_t before = refs.fetch_add(1, std::memory_order_relaxed);
      assert((before & COUNT) != COUNT);
    }

    // A count for a handle of an entry found by key. True if its load was
    // parked and this thread took it: it has to start the load again.
    [[nodiscard]] bool acquireFound() noexcept {
      const uint32_t before = refs.fetch_add(1, std::memory_order_relaxed);
      assert((before & COUNT) != COUNT);
      // Acquire: the parking task's claim of the entry is over.
      return (before & PARKED) != 0 && (refs.fetch_and(~PARKED, std::memory_order_acquire) & PARKED) != 0;
    }

    // The last handle puts the entry on the unused list of its owner, once.
    void release() noexcept {
      uint32_t before = refs.load(std::memory_order_relaxed);
      uint32_t after;
      do {
        assert((before & COUNT) != 0);
        after = before - 1;
        if ((after & COUNT) == 0) {
          after |= QUEUED;
        }
      } while (!refs.compare_exchange_weak(before, after, std::memory_order_acq_rel, std::memory_order_relaxed));
      if ((before & QUEUED) == 0 && (after & QUEUED) != 0) {
        // A push only and collect takes the whole list, so no ABA.
        ResourceEntry *head = unused->load(std::memory_order_relaxed);
        do {
          nextUnused = head;
        } while (!unused->compare_exchange_weak(head, this, std::memory_order_release, std::memory_order_relaxed));
      }
    }

    // For collect, under every lock of the cache, so that no bind can give it
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
        if (refs.compare_exchange_weak(now, now & ~QUEUED, std::memory_order_acq_rel, std::memory_order_acquire)) {
          return false;
        }
      }
    }

    // Takes the load left in the queue of the pool, if nobody has yet, and
    // runs it (Load) or lets it fail (Fail). True if this thread took it.
    bool claim(PendingRun run) noexcept {
      if (claimed.load(std::memory_order_relaxed) || claimed.exchange(true, std::memory_order_acq_rel)) {
        return false;
      }
      runPending(pending, run);
      return true;
    }

    // For the task, which holds a count: takes the load like claim(Load),
    // but leaves it in the entry and parks it if the count of the task is
    // the only one, so that a load nobody waits for any more costs nothing.
    void claimOrPark() noexcept {
      if (claimed.load(std::memory_order_relaxed) || claimed.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      // QUEUED may be set still: collect clears it lazily.
      uint32_t now = refs.load(std::memory_order_relaxed);
      while ((now & COUNT) == 1) {
        if (refs.compare_exchange_weak(now, now | PARKED, std::memory_order_acq_rel, std::memory_order_relaxed)) {
          return;
        }
      }
      runPending(pending, PendingRun::Load);
    }

    // For the thread that took PARKED: lets the next claim have the load.
    void unpark() noexcept { claimed.store(false, std::memory_order_release); }

    [[nodiscard]] bool parked() const noexcept { return (refs.load(std::memory_order_relaxed) & PARKED) != 0; }

    // Until the load of the entry is over. A load that has not started yet is
    // done here, so that threads of the pool that all wait for it do not wait
    // for a task none of them is free to run. Otherwise a thread that waits
    // joins a parallelFor of another thread meanwhile, the loader's among
    // them, and only sleeps if there is none. Throws what the loader threw.
    void wait() {
      claim(PendingRun::Load);
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
  //   ...
  //   if (material.albedo.ready()) { draw(*material.albedo); }
  //
  // ResourceCache::bind gives one, once; it stays with its resource while
  // that loads, is reloaded by forceLoad or fails.
  //
  // A copy counts itself in with one atomic add, a move costs nothing. When
  // the last handle of a resource goes, nothing is freed yet: the resource
  // waits for collect of its owner, see ResourceCache.
  //
  // A handle comes before its resource: ready() says when it is there, and
  // only then may it be used.
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

    // Whether the resource is loaded and may be used. Not yet while it loads,
    // never if its loader threw.
    [[nodiscard]] bool ready() const noexcept {
      return m_entry != nullptr && m_entry->state.load(std::memory_order_acquire) == internal::ResourceState::Ready;
    }

    // Whether its loader threw. The cache keeps the failed resource, so that
    // binding the key again does not load it again; forceLoad does.
    [[nodiscard]] bool failed() const noexcept {
      return m_entry != nullptr && m_entry->state.load(std::memory_order_acquire) == internal::ResourceState::Failed;
    }

    // Until the resource is loaded, and throws what its loader threw. A load
    // that has not started is done here.
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

  // Resources shared by key, each loaded once on the pool, as entt's
  // resource_cache, and given back only where their owner says.
  //
  //   ResourceCache<Texture> textures([](h64 key) { return Texture(assets.path(key)); });
  //   scheduler.provide(textures);
  //
  //   // once, where a material is made
  //   Material material{textures.bind("textures/rock"_h)};
  //   // every frame
  //   if (material.albedo.ready()) { draw(*material.albedo); }
  //
  //   scheduler.add([](Resources<Texture> textures) { textures->collect(); }, "collect textures");
  //
  // bind gives the handle of a key right away. If the key is new, it starts
  // the load on the pool, with the loader of the cache or the one it is
  // given; the handle is ready() once it is done. Every bind of the key gets
  // the same resource, also from threads that bind it at the same time.
  //
  // Which members may run at the same time:
  //
  //   - bind, load, size and everything of a ResourceHandle next to
  //     everything, on any thread, also in a loader. bind takes a lock,
  //     of the calling thread for a key apply has taken in, so it is for
  //     getting a handle once, not for every frame.
  //   - find and get next to each other and to the above, without a lock
  //     for keys apply has taken in. get writes nothing at all.
  //   - forceLoad, apply and collect next to none of find and get. The
  //     scheduler keeps the systems that take Resources<T> apart from those
  //     with Resources<const T>.
  //
  // A resource whose last handle went is given back only by collect, on the
  // thread that calls it, and not where the last component that had it was
  // destroyed. Until then bind gives it out again without loading it anew.
  // A load whose handles all went before its task started is not done: the
  // task leaves it in the resource, and the next bind or find of the key
  // starts it again.
  // Every resource has an owner, a number below the one given to the
  // constructor, which bind gives it, and collect(owner) leaves the
  // resources of other owners alone: for resources that go at points of
  // their own.
  //
  // A load that throws leaves a failed resource behind, for whoever has its
  // handle to see (failed()) and so that binding the key again does not load
  // it again; forceLoad does, and once nobody has it, collect gives it back
  // as any other.
  //
  // The cache knows no frames. What the GPU may still use has to wait for
  // the fence of its frame: the destructor of such a resource only puts it
  // into a memory::DeletionQueue, and collect may give it back any time.
  //
  // The key is the asset id. With the default, a 64-bit hash such as
  // sc::hash of package path and object name, two of n assets have the same
  // one with a chance of about n^2 / 2^65, 3e-8 for a million. A key of the
  // full name rules that out. TransparentHash takes an h64 key as its own
  // hash, so a lookup hashes nothing; a string key it hashes with sc::hash.
  template<typename T, typename Key = h64, typename Hash = TransparentHash>
  class ResourceCache {
    using Base = internal::ResourceEntry<T>;

    // The key is needed again when collect takes the entry out of the map.
    struct Entry : Base {
      Key key;

      Entry(Key key, uint8_t owner, std::atomic<Base *> *unused) : Base(owner, unused), key(std::move(key)) {}
    };

    using Map = ankerl::unordered_dense::map<Key, Entry *, Hash>;
    // The same for every map with this Hash: one hash finds the shard and the
    // key in both maps.
    using KeyHash = Map::precomputed_hash;

  public:
    // What bind(key) loads with: called with the key on the pool, on several
    // threads at once.
    using DefaultLoader = std::move_only_function<T(const Key &) const>;

    // Without a loader of its own, bind takes one every time.
    explicit ResourceCache(uint8_t owners = 1) : ResourceCache(nullptr, owners) {}

    explicit ResourceCache(DefaultLoader loader, uint8_t owners = 1) :
        m_owners(owners), m_unused(std::make_unique<UnusedList[]>(owners)),
        m_readers(std::make_unique<ReaderLock[]>(READERS)), m_shards(std::make_unique<Shard[]>(SHARDS)),
        m_loader(std::move(loader)) {
      assert(owners != 0);
    }

    // Everything there is goes. Waits for the loads on the pool first. No
    // handle may be left, and no member running.
    ~ResourceCache() {
      {
        std::unique_lock lock(m_taskMutex);
        m_tasksDone.wait(lock, [this] { return m_tasks == 0; });
      }
      const auto destroyAll = [](const Map &map) {
        for (const auto &[key, entry]: map) {
          assert((entry->refs.load(std::memory_order_relaxed) & Base::COUNT) == 0);
          assert(entry->state.load(std::memory_order_relaxed) != internal::ResourceState::Loading || entry->parked());
          destroy(entry);
        }
      };
      destroyAll(m_published);
      for (size_t shard = 0; shard < SHARDS; ++shard) {
        destroyAll(m_shards[shard].fresh);
      }
    }

    ResourceCache(const ResourceCache &) = delete;
    ResourceCache &operator=(const ResourceCache &) = delete;

    // --- next to everything ------------------------------------------------

    // The resource of the key, loaded with the loader of the cache if it is
    // new, for owner 0.
    [[nodiscard]] ResourceHandle<T> bind(const Key &key) const { return bind(key, uint8_t{0}); }

    // The same for the given owner. std::logic_error for a cache without a
    // loader of its own.
    [[nodiscard]] ResourceHandle<T> bind(const Key &key, uint8_t owner) const {
      if (!m_loader) {
        throw std::logic_error("ResourceCache: bind without a loader on a cache that has none");
      }
      return bind(key, owner, [this, key] { return m_loader(key); });
    }

    // The resource of the key, loaded with a copy of loader if it is new, for
    // owner 0.
    template<typename Loader>
      requires ResourceLoader<std::decay_t<Loader>, T> && std::constructible_from<std::decay_t<Loader>, Loader>
    [[nodiscard]] ResourceHandle<T> bind(const Key &key, Loader &&loader) const {
      return bind(key, 0, std::forward<Loader>(loader));
    }

    // The resource of the key right away, loaded or not. If the key is new,
    // a task of the pool calls a copy of loader, and the handle is ready()
    // once it is done, failed() if loader threw; a thread that waits for it
    // before the task starts calls the loader itself, and without workers in
    // the pool it is called here. The owner belongs to the resource: if it is
    // there already, it keeps the one it was loaded with, and debug builds
    // assert that it is the same.
    //
    // A key apply has taken in costs a lock of the calling thread only, which
    // apply, collect and forceLoad take too, so threads that bind one key
    // meet only on its count. A key bound since takes the lock of its shard;
    // other keys mostly have other locks.
    //
    // A load whose task finds every handle gone before it starts is not done:
    // the next bind or find of the key starts it again.
    //
    // threading::shutdown drops tasks that have not started: their
    // resources fail.
    template<typename Loader>
      requires ResourceLoader<std::decay_t<Loader>, T> && std::constructible_from<std::decay_t<Loader>, Loader>
    [[nodiscard]] ResourceHandle<T> bind(const Key &key, uint8_t owner, Loader &&loader) const {
      assert(owner < m_owners);
      using Job = PendingLoad<std::decay_t<Loader>>;
      const KeyHash hash = m_published.hash_for(key);
      if (ResourceHandle<T> published = bindPublished(key, hash)) {
        assert(published.m_entry->owner == owner);
        return published;
      }
      Shard &shard = shardOf(hash);
      Entry *entry;
      bool made = false;
      bool restart = false;
      {
        const std::scoped_lock lock(shard.mutex);
        entry = findLocked(shard, key, hash);
        if (entry != nullptr) {
          assert(entry->owner == owner);
          restart = entry->acquireFound();
        } else {
          made = true;
          auto job = std::make_unique<Job>(std::forward<Loader>(loader));
          auto fresh = std::make_unique<Entry>(key, owner, &m_unused[owner].head);
          shard.fresh.emplace(key, fresh.get());
          m_freshCount.fetch_add(1, std::memory_order_relaxed);
          entry = fresh.release();
          job->entry = entry;
          entry->runPending = &Job::run;
          entry->pending = job.release();
          // One count for the caller, one for the task.
          entry->acquire();
        }
      }
      ResourceHandle<T> handle(entry);
      // Not under the lock: without workers the loader runs here, and it may
      // bind keys of the same shard.
      if (made) {
        startTask(entry);
      } else if (restart) {
        startParked(entry);
      }
      return handle;
    }

    // The resource of the key, loaded by loader() on the calling thread if
    // it is new, for owner 0.
    template<typename Loader>
      requires ResourceLoader<Loader, T>
    ResourceHandle<T> load(const Key &key, Loader &&loader) const {
      return load(key, 0, loader);
    }

    // As bind, but it waits: a new key is loaded here, without a task, and
    // one that is loading is waited for. Throws what the loader threw, also
    // for a key whose load failed before.
    template<typename Loader>
      requires ResourceLoader<Loader, T>
    ResourceHandle<T> load(const Key &key, uint8_t owner, Loader &&loader) const {
      assert(owner < m_owners);
      const KeyHash hash = m_published.hash_for(key);
      Shard &shard = shardOf(hash);
      Entry *entry;
      bool made = false;
      bool restart = false;
      {
        const std::scoped_lock lock(shard.mutex);
        entry = findLocked(shard, key, hash);
        if (entry == nullptr) {
          auto fresh = std::make_unique<Entry>(key, owner, &m_unused[owner].head);
          fresh->claimed.store(true, std::memory_order_relaxed);
          shard.fresh.emplace(key, fresh.get());
          m_freshCount.fetch_add(1, std::memory_order_relaxed);
          entry = fresh.release();
          made = true;
        } else {
          assert(entry->owner == owner);
          restart = entry->acquireFound();
        }
      }
      // Not under the lock: the loader may bind keys of the same shard.
      if (made) {
        return fill(entry, loader);
      }
      ResourceHandle<T> handle(entry);
      if (restart) {
        // A parked load is done by the wait below, without a task.
        entry->unpark();
      }
      entry->wait();
      return handle;
    }

    // How many keys have a resource, loaded, loading or failed, also ones
    // that wait for collect.
    [[nodiscard]] size_t size() const {
      const AllLocks lock(*this);
      size_t count = m_published.size();
      for (size_t shard = 0; shard < SHARDS; ++shard) {
        count += m_shards[shard].fresh.size();
      }
      return count;
    }

    [[nodiscard]] uint8_t owners() const noexcept { return m_owners; }

    // --- next to each other, not next to the members below -----------------

    // The resource of the key, loaded or not, or none if it is not there.
    // Waits for nothing; the handle's wait does. Without a lock for a key
    // apply has taken in, with the lock of its shard for one bound since.
    [[nodiscard]] ResourceHandle<T> find(const Key &key) const {
      const KeyHash hash = m_published.hash_for(key);
      Entry *entry;
      bool restart;
      if (const auto found = m_published.find(key, hash); found != m_published.end()) {
        entry = found->second;
        restart = entry->acquireFound();
      } else {
        Shard &shard = shardOf(hash);
        const std::scoped_lock lock(shard.mutex);
        const auto fresh = shard.fresh.find(key, hash);
        if (fresh == shard.fresh.end()) {
          return {};
        }
        entry = fresh->second;
        restart = entry->acquireFound();
      }
      ResourceHandle<T> handle(entry);
      // Not under the lock, as in bind.
      if (restart) {
        startParked(entry);
      }
      return handle;
    }

    // The resource of the key if it is loaded and apply has taken the key
    // in, else null. Writes nothing, so threads that all get the same key do
    // not meet on a cache line. Keeps nothing either: the pointer is good
    // until the next forceLoad or collect; in a system, until the end of its
    // stage.
    [[nodiscard]] const T *get(const Key &key) const {
      const auto found = m_published.find(key);
      if (found == m_published.end() ||
          found->second->state.load(std::memory_order_acquire) != internal::ResourceState::Ready) {
        return nullptr;
      }
      return &found->second->value();
    }

    // --- not next to find and get ------------------------------------------

    // Loads the resource of the key anew, for owner 0.
    template<typename Loader>
      requires ResourceLoader<Loader, T>
    ResourceHandle<T> forceLoad(const Key &key, Loader &&loader) {
      return forceLoad(key, 0, loader);
    }

    // Loads the resource of the key anew, on the calling thread, into the
    // one there is: every handle of it sees the new one once this returns.
    // A load of it that is not over is waited for first. Throws what loader
    // threw; the resource is failed then.
    //
    // Nobody may use the old one meanwhile: the scheduler keeps systems
    // apart, not a loader on the pool that holds a handle of it.
    template<typename Loader>
      requires ResourceLoader<Loader, T>
    ResourceHandle<T> forceLoad(const Key &key, uint8_t owner, Loader &&loader) {
      assert(owner < m_owners);
      Base *entry;
      bool made = false;
      bool restart = false;
      {
        const AllLocks lock(*this);
        const KeyHash hash = m_published.hash_for(key);
        entry = findLocked(shardOf(hash), key, hash);
        if (entry == nullptr) {
          auto fresh = std::make_unique<Entry>(key, owner, &m_unused[owner].head);
          fresh->claimed.store(true, std::memory_order_relaxed);
          m_published.emplace(key, fresh.get());
          entry = fresh.release();
          made = true;
        } else {
          assert(entry->owner == owner);
          restart = entry->acquireFound();
        }
      }
      // Not under the locks: the loader may bind other keys.
      if (made) {
        return fill(entry, loader);
      }
      ResourceHandle<T> handle(entry);
      if (restart) {
        entry->unpark();
        // The parked load would only be replaced below.
        if (entry->claim(internal::PendingRun::Free)) {
          entry->acquire();
          (void) fill(entry, loader);
          return handle;
        }
      }
      try {
        entry->wait();
        // NOLINTNEXTLINE(bugprone-empty-catch): the failed one is loaded anew below
      } catch (...) {
      }
      if (entry->state.load(std::memory_order_relaxed) == internal::ResourceState::Ready) {
        entry->value().~T();
      }
      entry->error = nullptr;
      entry->state.store(internal::ResourceState::Loading, std::memory_order_relaxed);
      entry->acquire();
      (void) fill(entry, loader);
      return handle;
    }

    // Takes the keys bound since the last apply into the map that find and
    // get look into without a lock, and returns how many. The scheduler
    // calls it after every stage; without a new key it takes no lock.
    size_t apply() {
      if (m_freshCount.load(std::memory_order_relaxed) == 0) {
        return 0;
      }
      const AllLocks lock(*this);
      m_freshCount.store(0, std::memory_order_relaxed);
      size_t moved = 0;
      for (size_t shard = 0; shard < SHARDS; ++shard) {
        Map &fresh = m_shards[shard].fresh;
        moved += fresh.size();
        for (const auto &[key, entry]: fresh) {
          m_published.emplace(key, entry);
        }
        fresh.clear();
      }
      return moved;
    }

    // Gives back the resources of the owner that have had no handle since
    // their last one went, and returns how many. Their destructors run here,
    // on the calling thread, without a lock.
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
        const AllLocks lock(*this);
        while (list != nullptr) {
          Base *const entry = std::exchange(list, list->nextUnused);
          if (!entry->unusedNow()) {
            continue;
          }
          const Key &key = static_cast<Entry *>(entry)->key;
          const KeyHash hash = m_published.hash_for(key);
          if (const auto found = m_published.find(key, hash); found != m_published.end()) {
            m_published.erase(found);
          } else {
            shardOf(hash).fresh.erase(key);
          }
          entry->nextUnused = dead;
          dead = entry;
        }
      }
      size_t count = 0;
      while (dead != nullptr) {
        destroy(std::exchange(dead, dead->nextUnused));
        ++count;
      }
      return count;
    }

  private:
    struct alignas(64) UnusedList {
      std::atomic<Base *> head{nullptr};
    };

    // The keys bound since the last apply, split by hash, each part with a
    // lock of its own, so that threads that bind different keys do not wait
    // for each other.
    static constexpr uint32_t SHARD_BITS = 6;
    static constexpr size_t SHARDS = size_t{1} << SHARD_BITS;
    // Bits 0 to 7 of the hash are the fingerprint of a map, the groups start
    // at the top; these lie between for any map below 2^50 groups.
    static constexpr uint32_t SHARD_SHIFT = 8;

    struct alignas(64) Shard {
      std::mutex mutex;
      Map fresh;
    };

    // A lock for each thread of the pool with a threadId below READERS, under
    // which bind looks into m_published: threads that bind the same key do
    // not meet on a lock. The other threads take the lock of the shard, under
    // which m_published may be read as well.
    static constexpr size_t READERS = 64;

    struct alignas(64) ReaderLock {
      std::mutex mutex;
    };

    // Every lock, in order: what changes m_published.
    class AllLocks {
    public:
      explicit AllLocks(const ResourceCache &cache) : m_readers(cache.m_readers.get()), m_shards(cache.m_shards.get()) {
        for (size_t reader = 0; reader < READERS; ++reader) {
          m_readers[reader].mutex.lock();
        }
        for (size_t shard = 0; shard < SHARDS; ++shard) {
          m_shards[shard].mutex.lock();
        }
      }

      ~AllLocks() {
        for (size_t shard = SHARDS; shard-- > 0;) {
          m_shards[shard].mutex.unlock();
        }
        for (size_t reader = READERS; reader-- > 0;) {
          m_readers[reader].mutex.unlock();
        }
      }

      AllLocks(const AllLocks &) = delete;
      AllLocks &operator=(const AllLocks &) = delete;

    private:
      ReaderLock *m_readers;
      Shard *m_shards;
    };

    // The load left for later, with a copy of the loader. The thread that
    // claims the entry calls run once, which frees it.
    template<typename Loader>
    struct PendingLoad {
      Entry *entry = nullptr;
      Loader loader;

      template<typename From>
        requires(!std::same_as<std::remove_cvref_t<From>, PendingLoad>)
      explicit PendingLoad(From &&loader) : loader(std::forward<From>(loader)) {}

      // noexcept on purpose: if fail throws (no memory for the error), the
      // entry would stay loading and whoever waits for it would wait for
      // ever; the program ends instead.
      // NOLINTNEXTLINE(bugprone-exception-escape): see above
      static void run(void *pending, internal::PendingRun run) noexcept {
        const std::unique_ptr<PendingLoad> job(static_cast<PendingLoad *>(pending));
        switch (run) {
          case internal::PendingRun::Load:
            job->entry->acquire();
            try {
              (void) fill(job->entry, job->loader);
              // NOLINTNEXTLINE(bugprone-empty-catch): fail kept it in the entry for whoever waits or looks
            } catch (...) {
            }
            break;
          case internal::PendingRun::Fail:
            fail(job->entry, std::make_exception_ptr(
                                 std::runtime_error("ResourceCache: the task of a load was dropped before it ran")));
            break;
          case internal::PendingRun::Free:
            break;
        }
      }
    };

    // The task of bind on the pool. Holds one count of the entry and is
    // counted in m_tasks until it goes, whether it ran or was dropped.
    class Task {
    public:
      Task(const ResourceCache *cache, Base *entry) noexcept : m_cache(cache), m_entry(entry) {}
      Task(Task &&other) noexcept :
          m_cache(other.m_cache), m_entry(std::exchange(other.m_entry, nullptr)), m_ran(other.m_ran) {}
      Task &operator=(Task &&) = delete;

      ~Task() {
        if (m_entry != nullptr) {
          // Only if it was dropped: once it ran, a load it parked may have
          // been given to the next claim already.
          if (!m_ran) {
            m_entry->claim(internal::PendingRun::Fail);
          }
          m_entry->release();
          m_cache->taskDone();
        }
      }

      void operator()() noexcept {
        m_ran = true;
        m_entry->claimOrPark();
      }

    private:
      const ResourceCache *m_cache;
      Base *m_entry;
      bool m_ran = false;
    };

    [[nodiscard]] Shard &shardOf(KeyHash hash) const noexcept {
      return m_shards[(hash.m_mixed_hash >> SHARD_SHIFT) & (SHARDS - 1)];
    }

    // The entry of the key in either map, under the lock of its shard or
    // all of them.
    [[nodiscard]] Entry *findLocked(Shard &shard, const Key &key, KeyHash hash) const {
      if (const auto found = m_published.find(key, hash); found != m_published.end()) {
        return found->second;
      }
      if (const auto found = shard.fresh.find(key, hash); found != shard.fresh.end()) {
        return found->second;
      }
      return nullptr;
    }

    // The handle of a key apply has taken in, under the reader lock of the
    // calling thread; none for any other key, and for every key on a thread
    // without a reader lock.
    [[nodiscard]] ResourceHandle<T> bindPublished(const Key &key, KeyHash hash) const {
      const uint32_t thread = threading::threadId();
      if (thread >= READERS) {
        return {};
      }
      Entry *entry;
      bool restart;
      {
        const std::scoped_lock lock(m_readers[thread].mutex);
        const auto found = m_published.find(key, hash);
        if (found == m_published.end()) {
          return {};
        }
        entry = found->second;
        restart = entry->acquireFound();
      }
      ResourceHandle<T> handle(entry);
      // Not under the lock: without workers the loader runs here, and it may
      // bind keys too.
      if (restart) {
        startParked(entry);
      }
      return handle;
    }

    // Starts the load of an entry whose PARKED this thread took, as a new
    // task with a count of its own.
    void startParked(Base *entry) const {
      entry->acquire();
      entry->unpark();
      startTask(entry);
    }

    // Runs the pending load of the entry as a task, which takes over one
    // count of it.
    void startTask(Base *entry) const {
      {
        const std::scoped_lock lock(m_taskMutex);
        ++m_tasks;
      }
      Task task(this, entry);
      if (threading::threadSlots() <= 1) {
        task();
      } else {
        threading::detach(std::move(task));
      }
    }

    void taskDone() const noexcept {
      const std::scoped_lock lock(m_taskMutex);
      if (--m_tasks == 0) {
        m_tasksDone.notify_all();
      }
    }

    // Calls the loader for an entry that is loading, and takes over one count
    // of it for the handle it returns. Touches nothing but the entry, so a
    // task may do it next to any member.
    template<typename Loader>
    static ResourceHandle<T> fill(Base *entry, Loader &loader) {
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

    // The failed entry stays, see the class.
    static void fail(Base *entry, std::exception_ptr error) noexcept {
      entry->error = std::move(error);
      entry->state.store(internal::ResourceState::Failed, std::memory_order_release);
      entry->state.notify_all();
    }

    static void destroy(Base *entry) noexcept {
      if (entry->state.load(std::memory_order_relaxed) == internal::ResourceState::Ready) {
        entry->value().~T();
      }
      if (entry->parked()) {
        entry->runPending(entry->pending, internal::PendingRun::Free);
      }
      delete static_cast<Entry *>(entry);
    }

    uint8_t m_owners;
    std::unique_ptr<UnusedList[]> m_unused;
    // The keys apply has taken in. Changed only under every lock, by the
    // members that do not run next to find and get, so those read it without
    // one.
    Map m_published;
    std::unique_ptr<ReaderLock[]> m_readers;
    std::unique_ptr<Shard[]> m_shards;
    DefaultLoader m_loader;
    // Keys put into a shard since the last apply. Changed under the lock of
    // the shard, read by apply without one: one bound while apply looks is
    // taken in by the next.
    mutable std::atomic<size_t> m_freshCount{0};
    mutable std::mutex m_taskMutex;
    // Loads of bind on the pool that have not gone yet, under m_taskMutex.
    mutable size_t m_tasks = 0;
    mutable std::condition_variable m_tasksDone;
  };

} // namespace sc::ecs
