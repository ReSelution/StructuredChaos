#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

#include <ankerl/unordered_dense.h>

#include "sc/hash/hash.hpp"
#include "sc/io/file_reader.hpp"
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

  // What a ResourceCache of T makes one of from the bytes of a file, the
  // same way.
  template<typename Build, typename T>
  concept ResourceBuilder = std::invocable<Build &, std::span<const std::byte>> &&
                            (std::same_as<std::invoke_result_t<Build &, std::span<const std::byte>>, T> ||
                             std::constructible_from<T, std::invoke_result_t<Build &, std::span<const std::byte>>>);

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
    // False while its load waits in the queue of the lane: the first thread
    // to set it calls pending, the task or one that waits for the resource.
    // Also true while it is parked; the thread that takes PARKED clears it.
    std::atomic<bool> claimed{false};
    uint8_t owner;
    ResourceEntry *nextUnused = nullptr;
    // While its load waits for a place on the lane, see
    // ResourceCache::dispatch; not nextUnused, as its last handle may queue
    // it meanwhile.
    ResourceEntry *nextWaiting = nullptr;
    std::atomic<ResourceEntry *> *unused;
    // The load left for later: Load loads, Fail lets it fail without calling
    // the loader, Free only frees it, for a parked entry that goes. Each frees
    // pending.
    void (*runPending)(void *pending, PendingRun run) noexcept = nullptr;
    void *pending = nullptr;
    // The reader that reads the file of the entry, while it does; claimed
    // is true meanwhile, and pending is set only once the bytes are there.
    std::atomic<io::FileReader *> reading{nullptr};
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

    // Takes the load left in the queue of the lane, if nobody has yet, and
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
    // done here, so that a thread that waits for it does not wait behind
    // other tasks of the lane. Otherwise a thread that waits joins a
    // parallelFor of the frame meanwhile, unless it is a thread of the lane,
    // and only sleeps if there is none; a parallelFor of a loader on the lane
    // it does not join, see sc::threading::helpThread. Throws what the loader
    // threw.
    //
    // While its file is read, it waits in the reader's wait instead: the
    // bytes come only to whoever polls it.
    void wait() {
      while (true) {
        claim(PendingRun::Load);
        if (state.load(std::memory_order_acquire) != ResourceState::Loading) {
          break;
        }
        if (io::FileReader *const files = reading.load(std::memory_order_acquire)) {
          // 0 while another thread is in the done of this one.
          if (files->wait() == 0) {
            std::this_thread::yield();
          }
          continue;
        }
        const auto loaded = [this] { return state.load(std::memory_order_acquire) != ResourceState::Loading; };
        // A loader of the lane that waits for another resource does not join
        // the frame: at SCHED_IDLE, the frame would wait for its chunk until
        // it gets a core.
        if (threading::onBackgroundThread() || !threading::helpThread(loaded)) {
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

  // Resources shared by key, each loaded once on the background lane, as entt's
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
  // the load on the background lane (sc::threading::spawn), with the loader
  // of the cache or the one it is
  // given; the handle is ready() once it is done. Every bind of the key gets
  // the same resource, also from threads that bind it at the same time.
  //
  // Not on the pool: a loader that waits for the disk or decodes for long
  // would keep a worker from the parallelFor of a frame. At most maxLoads of
  // these loads run on the lane at once, half of its threads by default; the
  // others wait in the cache, not in the queue of the lane, so that loads
  // leave the lane to other tasks as well.
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
    // What bind(key) loads with: called with the key on the lane, on several
    // threads at once.
    using DefaultLoader = std::move_only_function<T(const Key &) const>;
    // What bind(key) reads and builds with instead, given both: the path of
    // the file of a new key, and what makes the T of its bytes, on the lane,
    // on several threads at once.
    using FilePath = std::move_only_function<std::string(const Key &) const>;
    using DefaultBuilder = std::move_only_function<T(std::span<const std::byte>) const>;

    // Without a loader of its own, bind takes one every time.
    explicit ResourceCache(uint8_t owners = 1, size_t maxLoads = 0) :
        ResourceCache(nullptr, nullptr, nullptr, owners, maxLoads) {}

    // maxLoads: how many loads of bind run on the lane at once; 0 for half of
    // the threads it has when a load starts, at least one.
    explicit ResourceCache(DefaultLoader loader, uint8_t owners = 1, size_t maxLoads = 0) :
        ResourceCache(std::move(loader), nullptr, nullptr, owners, maxLoads) {}

    // bind(key) reads the file at path(key) through the cache's reader and
    // makes the resource with build, see bind with a file. path is called
    // only for a key that is not there yet.
    ResourceCache(FilePath path, DefaultBuilder build, uint8_t owners = 1, size_t maxLoads = 0) :
        ResourceCache(nullptr, std::move(path), std::move(build), owners, maxLoads) {}

    // Everything there is goes. Waits for the loads on the lane first. No
    // handle may be left, and no member running.
    ~ResourceCache() {
      // Reads of files come only with a poll of the reader, which nobody
      // else may do any more. The done of a read counts its build in before
      // it counts the read out, so the count of tasks only falls to 0 once
      // the reads are over.
      while (m_reads.load(std::memory_order_acquire) != 0) {
        if (m_files->wait() == 0) {
          std::this_thread::yield();
        }
      }
      for (size_t now = m_tasks.load(std::memory_order_acquire); now != 0;
           now = m_tasks.load(std::memory_order_acquire)) {
        m_tasks.wait(now, std::memory_order_acquire);
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

    // The resource of the key, if it is new loaded with the loader of the
    // cache, or read from the file at its path and built, for owner 0.
    [[nodiscard]] ResourceHandle<T> bind(const Key &key) const { return bind(key, uint8_t{0}); }

    // The same for the given owner. std::logic_error for a cache with
    // neither.
    [[nodiscard]] ResourceHandle<T> bind(const Key &key, uint8_t owner) const {
      if (!m_loader) {
        return bindWithFilePath(key, owner);
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
    // a task of the lane calls a copy of loader, and the handle is ready()
    // once it is done, failed() if loader threw; with maxLoads running, the
    // task waits in the cache first. A thread that waits for it before the
    // task starts calls the loader itself, and without threads in the lane it
    // is called here. The owner belongs to the resource: if it is
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
    // threading::shutdownBackground drops tasks that have not started, those
    // that wait in the cache too: their resources fail.
    template<typename Loader>
      requires ResourceLoader<std::decay_t<Loader>, T> && std::constructible_from<std::decay_t<Loader>, Loader>
    [[nodiscard]] ResourceHandle<T> bind(const Key &key, uint8_t owner, Loader &&loader) const {
      assert(owner < m_owners);
      const KeyHash hash = m_published.hash_for(key);
      if (ResourceHandle<T> published = bindPublished(key, hash)) {
        assert(published.m_entry->owner == owner);
        return published;
      }
      using Job = PendingLoad<std::decay_t<Loader>>;
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
      // Not under the lock: without a lane the loader runs here, and it may
      // bind keys of the same shard.
      if (made) {
        startTask(entry);
      } else if (restart) {
        startParked(entry);
      }
      return handle;
    }

    // The resource of the key, made by build from the bytes of the file at
    // path if it is new, for owner 0.
    template<typename Build>
      requires ResourceBuilder<std::decay_t<Build>, T> && std::constructible_from<std::decay_t<Build>, Build>
    [[nodiscard]] ResourceHandle<T> bind(const Key &key, std::string path, Build &&build) const {
      return bind(key, 0, std::move(path), std::forward<Build>(build));
    }

    // As bind with a loader, but in two steps if the key is new: the cache's
    // io::FileReader reads the file at path, without a thread that waits for
    // it, and once a poll of it has the bytes, a task of the lane calls a
    // copy of build with them, under maxLoads as any load; without a lane,
    // in that poll. The handle is failed() with a std::system_error if the
    // file cannot be read.
    //
    // The reader is made by the first bind with a file, so a cache without
    // one has no io_uring; std::system_error if the kernel has none for us.
    // apply polls it, which the scheduler calls after every stage, and so
    // does the wait of a handle while its file is read; through io_uring the
    // bytes are there after two polls at the earliest, one for the open and
    // one for the read.
    template<typename Build>
      requires ResourceBuilder<std::decay_t<Build>, T> && std::constructible_from<std::decay_t<Build>, Build>
    [[nodiscard]] ResourceHandle<T> bind(const Key &key, uint8_t owner, std::string path, Build &&build) const {
      return bindFile(key, owner, [&path] { return std::move(path); }, std::forward<Build>(build));
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

    // How many files of bind are being read, whose bytes have not reached
    // the cache yet: for a loading screen, say.
    [[nodiscard]] size_t reads() const noexcept { return m_reads.load(std::memory_order_relaxed); }

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
    // apart, not a loader on the lane that holds a handle of it.
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
    // calls it after every stage; without a new key and without a read of
    // bind with a file under way it takes no lock.
    //
    // Polls the reader of those reads first, so that their bytes reach the
    // cache, and a build that runs in the poll (without a lane) may bind
    // keys, which are taken in right after.
    size_t apply() {
      pollFiles();
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
    ResourceCache(DefaultLoader loader, FilePath path, DefaultBuilder build, uint8_t owners, size_t maxLoads) :
        m_owners(owners), m_unused(std::make_unique<UnusedList[]>(owners)),
        m_readers(std::make_unique<ReaderLock[]>(READERS)), m_shards(std::make_unique<Shard[]>(SHARDS)),
        m_loader(std::move(loader)), m_filePath(std::move(path)), m_build(std::move(build)), m_maxLoads(maxLoads) {
      assert(owners != 0);
    }

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

    // The task of bind on the lane. Holds one count of the entry and is
    // counted in m_tasks until it goes, whether it ran or was dropped; on the
    // lane, also in m_running.
    class Task {
    public:
      Task(const ResourceCache *cache, Base *entry, bool onLane) noexcept :
          m_cache(cache), m_entry(entry), m_onLane(onLane) {}
      Task(Task &&other) noexcept :
          m_cache(other.m_cache), m_entry(std::exchange(other.m_entry, nullptr)), m_onLane(other.m_onLane),
          m_ran(other.m_ran) {}
      Task &operator=(Task &&) = delete;

      ~Task() {
        if (m_entry != nullptr) {
          // Only if it was dropped: once it ran, a load it parked may have
          // been given to the next claim already.
          if (!m_ran) {
            m_entry->claim(internal::PendingRun::Fail);
          }
          m_entry->release();
          m_cache->taskDone(m_onLane);
        }
      }

      void operator()() noexcept {
        m_ran = true;
        m_entry->claimOrPark();
      }

    private:
      const ResourceCache *m_cache;
      Base *m_entry;
      bool m_onLane;
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

    // bind(key, owner) of a cache with a path and a build instead of a
    // loader. Out of line: with it in bind(key, owner), bind of a key with
    // the loader of the cache took 470-490 ns instead of 300-315 on 8
    // threads in half of the runs (measured, threads pinned to cores; the
    // cause is not found, see the commit of bind with a FileReader).
    [[gnu::noinline]] ResourceHandle<T> bindWithFilePath(const Key &key, uint8_t owner) const {
      if (!m_build) {
        throw std::logic_error("ResourceCache: bind without a loader on a cache that has none");
      }
      // The path only for a key that is not there yet.
      return bindFile(
          key, owner, [this, &key] { return m_filePath(key); },
          [this](std::span<const std::byte> bytes) { return m_build(bytes); });
    }

    // bind with a file, with the path from pathOf, called only for a key
    // that is not there yet.
    template<typename PathOf, typename Build>
    [[nodiscard]] ResourceHandle<T> bindFile(const Key &key, uint8_t owner, PathOf &&pathOf, Build &&build) const {
      assert(owner < m_owners);
      const KeyHash hash = m_published.hash_for(key);
      if (ResourceHandle<T> published = bindPublished(key, hash)) {
        assert(published.m_entry->owner == owner);
        return published;
      }
      return bindFresh(key, hash, owner, std::forward<PathOf>(pathOf), std::forward<Build>(build));
    }

    // What bind with a file does after bindPublished.
    template<typename PathOf, typename Build>
    [[nodiscard]] ResourceHandle<T> bindFresh(const Key &key, KeyHash hash, uint8_t owner, PathOf &&pathOf,
                                              Build &&build) const {
      io::FileReader &reader = files();
      Entry *made = nullptr;
      ResourceHandle<T> handle = findOrMake(key, hash, owner, made, [&](Entry &entry) {
        // Nobody may claim the load before the bytes are there.
        entry.claimed.store(true, std::memory_order_relaxed);
        entry.reading.store(&reader, std::memory_order_relaxed);
        // One count for the caller, one for the read.
        entry.acquire();
      });
      if (made != nullptr) {
        startRead(made, reader, std::forward<PathOf>(pathOf), std::forward<Build>(build));
      }
      return handle;
    }

    // The rest of bind once bindPublished has found nothing, under the lock
    // of the key's shard: the handle of the key from either map, or of a new
    // entry, which prepare gives its load; made is that entry then, and the
    // caller starts its load outside the lock. Starts a parked load it finds
    // again.
    template<typename Prepare>
    [[nodiscard]] ResourceHandle<T> findOrMake(const Key &key, KeyHash hash, uint8_t owner, Entry *&made,
                                               Prepare &&prepare) const {
      Shard &shard = shardOf(hash);
      Entry *entry;
      bool restart = false;
      {
        const std::scoped_lock lock(shard.mutex);
        entry = findLocked(shard, key, hash);
        if (entry != nullptr) {
          assert(entry->owner == owner);
          restart = entry->acquireFound();
        } else {
          auto fresh = std::make_unique<Entry>(key, owner, &m_unused[owner].head);
          prepare(*fresh);
          try {
            shard.fresh.emplace(key, fresh.get());
          } catch (...) {
            if (fresh->pending != nullptr) {
              fresh->runPending(fresh->pending, internal::PendingRun::Free);
            }
            throw;
          }
          m_freshCount.fetch_add(1, std::memory_order_relaxed);
          entry = made = fresh.release();
        }
      }
      ResourceHandle<T> handle(entry);
      // Not under the lock, as the load of a new entry.
      if (restart) {
        startParked(entry);
      }
      return handle;
    }

    // What the task of a file read calls: build with the bytes.
    template<typename Build>
    struct BuildFromFile {
      io::FileBytes file;
      Build build;

      decltype(auto) operator()() { return build(file.bytes()); }
    };

    // Hands the file of a new entry to the reader, with a count of the entry
    // and counted in m_tasks until its done has run.
    template<typename PathOf, typename Build>
    void startRead(Entry *entry, io::FileReader &files, PathOf &&pathOf, Build &&build) const {
      m_tasks.fetch_add(1, std::memory_order_relaxed);
      // Release: whoever sees it above 0 sees m_files made.
      m_reads.fetch_add(1, std::memory_order_release);
      try {
        // A pathOf that throws fails the entry as a read that cannot start.
        std::string path = std::forward<PathOf>(pathOf)();
        files.read(path,
                   [this, entry, path, build = std::decay_t<Build>(std::forward<Build>(build))](
                       io::FileResult file) mutable { bytesRead(entry, std::move(file), path, std::move(build)); });
      } catch (...) {
        readOver(entry, std::current_exception());
      }
    }

    // The done of a read, on the thread that polls the reader. Once the
    // bytes are there, the load is left in the entry as for bind with a
    // loader, and its task takes over the count of the read.
    template<typename Build>
    void bytesRead(Base *entry, io::FileResult file, const std::string &path, Build build) const noexcept {
      if (!file) {
        readOver(entry, std::make_exception_ptr(std::system_error(file.error(), "ResourceCache: reading " + path)));
        return;
      }
      using Job = PendingLoad<BuildFromFile<Build>>;
      try {
        auto job = std::make_unique<Job>(BuildFromFile<Build>{.file = std::move(*file), .build = std::move(build)});
        job->entry = static_cast<Entry *>(entry);
        entry->runPending = &Job::run;
        entry->pending = job.release();
      } catch (...) {
        readOver(entry, std::current_exception());
        return;
      }
      // In this order for wait: once reading is null, the load can be
      // claimed.
      entry->unpark();
      entry->reading.store(nullptr, std::memory_order_release);
      // noexcept on purpose, as PendingLoad::run: a task that cannot be
      // queued for want of memory would leave the entry loading for ever.
      startTask(entry);
      readDone();
    }

    // A read that ends without bytes: the entry fails and lets go of the
    // count of the read.
    void readOver(Base *entry, std::exception_ptr error) const noexcept {
      fail(entry, std::move(error));
      entry->reading.store(nullptr, std::memory_order_release);
      entry->release();
      readDone();
    }

    void readDone() const noexcept {
      m_reads.fetch_sub(1, std::memory_order_relaxed);
      taskGone();
    }

    // The reader of bind with a file, made by the first one.
    [[nodiscard]] io::FileReader &files() const {
      std::call_once(m_filesMade, [this] { m_files = std::make_unique<io::FileReader>(); });
      return *m_files;
    }

    // For apply and the destructor: polls the reader while reads are under
    // way.
    void pollFiles() const {
      if (m_reads.load(std::memory_order_acquire) != 0) {
        (void) m_files->poll();
      }
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
      // Not under the lock: without a lane the loader runs here, and it may
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
    // count of it: on the lane once one of maxLoads places is free, see
    // dispatch. Without threads in the lane it runs here, and no limit counts
    // it.
    void startTask(Base *entry) const {
      m_tasks.fetch_add(1, std::memory_order_relaxed);
      if (threading::backgroundThreadCount() == 0) {
        Task(this, entry, false)();
        return;
      }
      // A push only, and dispatch takes the whole list, so no ABA.
      Base *head = m_incoming.load(std::memory_order_relaxed);
      do {
        entry->nextWaiting = head;
      } while (!m_incoming.compare_exchange_weak(head, entry, std::memory_order_release, std::memory_order_relaxed));
      dispatch();
    }

    // Starts loads that wait while fewer than maxLoads run, without a lock:
    // the threads of the lane run at SCHED_IDLE, and a system that binds a
    // key must not wait for one of them that holds a lock until it gets a
    // core. One thread dispatches at a time; whoever comes meanwhile leaves
    // a request, which the one that dispatches sees before it stops, and
    // returns. 2000 new keys bound per frame on every pool thread while the
    // lane finishes loads of 50 us (Ryzen 7 5800X, 15 workers, 4 threads of
    // the lane), median / p99 ms per frame: with one mutex for this 0.386-
    // 0.391 / 0.62-0.70, now 0.314-0.320 / 0.58-0.63; the loads take as long
    // as before. The outliers of 2-3 ms come either way, cause not looked
    // into.
    void dispatch() const noexcept {
      uint32_t handled = 1;
      if (m_dispatchRequests.fetch_add(1, std::memory_order_acq_rel) != 0) {
        return;
      }
      while (true) {
        dispatchOnce();
        const uint32_t left = m_dispatchRequests.fetch_sub(handled, std::memory_order_acq_rel) - handled;
        if (left == 0) {
          return;
        }
        handled = left;
      }
    }

    // For the thread that dispatches: moves what was pushed since to the end
    // of m_queued, in the order of startTask, and starts from its front.
    // Only this thread raises m_running, so no more than the limit start.
    void dispatchOnce() const noexcept {
      Base *pushed = m_incoming.exchange(nullptr, std::memory_order_acquire);
      // Newest first as pushed; reversed, the oldest goes first.
      Base *oldest = nullptr;
      Base *newest = pushed;
      while (pushed != nullptr) {
        Base *const next = pushed->nextWaiting;
        pushed->nextWaiting = oldest;
        oldest = pushed;
        pushed = next;
      }
      if (oldest != nullptr) {
        if (m_queuedTail != nullptr) {
          m_queuedTail->nextWaiting = oldest;
        } else {
          m_queuedHead = oldest;
        }
        m_queuedTail = newest;
      }
      while (m_queuedHead != nullptr && m_running.load(std::memory_order_acquire) < loadLimit()) {
        Base *const entry = m_queuedHead;
        m_queuedHead = entry->nextWaiting;
        if (m_queuedHead == nullptr) {
          m_queuedTail = nullptr;
        }
        entry->nextWaiting = nullptr;
        m_running.fetch_add(1, std::memory_order_relaxed);
        submit(Task(this, entry, true));
      }
    }

    // A load of bind or a read went: the destructor waits for none to be left.
    void taskGone() const noexcept {
      if (m_tasks.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        m_tasks.notify_all();
      }
    }

    // Without a stop: a load has no point to stop at, and one whose handles
    // all went is parked by its task anyway.
    static void submit(Task task) {
      threading::internal::submitBackground([task = std::move(task)](const std::stop_token &) mutable { task(); },
                                            std::stop_source(std::nostopstate));
    }

    // A task that ran or was dropped hands its place on the lane to the next
    // load that waits. One that shutdownBackground drops does so too: the
    // next is queued where shutdownBackground is emptying, and dropped in
    // turn.
    void taskDone(bool onLane) const noexcept {
      if (onLane) {
        m_running.fetch_sub(1, std::memory_order_acq_rel);
        dispatch();
      }
      taskGone();
    }

    // The maxLoads given to the constructor, or half the threads of the lane:
    // 500 loads bound at once next to 40 frames of a parallelFor of 1600 x
    // 10 us (Ryzen 7 5800X, 15 workers, 4 threads of the lane), median ms per
    // frame, ms for the loads and the frames, ms another task of the lane
    // spawned right after waits: without loads 1.16-1.22; loads that sleep
    // 2 ms, maxLoads 1 / 2 / 4: 1.22-1.27, 813-902, 0 / 1.26, 522, 0 / 1.24-
    // 1.25, 266-286, 2.0-2.4; loads that compute 2 ms: 1.29, 1030-1057, 0 /
    // 1.29-1.34, 566-568, 0 / 1.35-1.37, 303, 2.3-3.3. On the pool before,
    // a quarter of the workers (3), sleeping: 1.22-1.25 against 1.09-1.11
    // without, 353.
    [[nodiscard]] size_t loadLimit() const noexcept {
      if (m_maxLoads != 0) {
        return m_maxLoads;
      }
      return std::max<size_t>(1, threading::backgroundThreadCount() / 2);
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
    FilePath m_filePath;
    DefaultBuilder m_build;
    // Keys put into a shard since the last apply. Changed under the lock of
    // the shard, read by apply without one: one bound while apply looks is
    // taken in by the next.
    mutable std::atomic<size_t> m_freshCount{0};
    size_t m_maxLoads;
    // Loads of bind that have not gone yet, waiting ones too, and reads whose
    // done has not run.
    mutable std::atomic<size_t> m_tasks{0};
    // Tasks on the lane, raised only by the thread that dispatches.
    mutable std::atomic<size_t> m_running{0};
    // Loads that wait for a place on the lane with the count of their task:
    // pushed by startTask, newest first, and taken whole by dispatch.
    mutable std::atomic<Base *> m_incoming{nullptr};
    // Those dispatch took and could not start yet, oldest first; only the
    // thread that dispatches touches them.
    mutable Base *m_queuedHead = nullptr;
    mutable Base *m_queuedTail = nullptr;
    // Calls of dispatch not handled yet; the one that takes it from 0
    // dispatches.
    mutable std::atomic<uint32_t> m_dispatchRequests{0};
    mutable std::once_flag m_filesMade;
    mutable std::unique_ptr<io::FileReader> m_files;
    // Reads of bind with a file whose done has not run yet, so that apply
    // polls only while there are some.
    mutable std::atomic<size_t> m_reads{0};
  };

} // namespace sc::ecs
