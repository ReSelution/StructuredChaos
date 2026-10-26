# TODO

Open points first in every section; done ones are kept short, with their main number and commit. The full
measurements of the done ones are in the history of this file (before 2026-10-23).

Found while integrating `sc::ecs` into [ecs_benchmark](https://github.com/abeimler/ecs_benchmark)
(`src/structured-chaos`, pinned to commit `bfd4be4`).

## Decisions of the benchmark integration to review

- **Single thread:** the `World` wrapper of the benchmark calls `sc::threading::init(0)` once, so that the
  benchmark thread has a slot of its own in `EntityAllocator::withShare` instead of going through `m_sharedMutex`.
  Still needed at `25cf384`: a create that takes a new index costs 6.4 ns without it and 0.94 with it (65 536
  entities, clang; with 2 `emplace` 10.7 and 8.6). Indices from the free list skip `withShare`, which is why
  `BM_CreateEntities` (destroys between iterations) showed 10.34 against 10.29 at `b3e1fef`.
- **No malloc override:** mimalloc is built with `MI_OVERRIDE OFF`; ecs uses it only through its own `mi_*` calls,
  the rest of the benchmark runs on the system malloc like every other framework. `tests/ecs_compare` instead uses
  mimalloc for all libraries.
- **fmt from vcpkg:** spdlog is built with `SPDLOG_FMT_EXTERNAL`. `SPDLOG_USE_STD_FORMAT` fails because
  `sc/logger/logger.hpp:147` uses `fmt_lib::runtime`, which `std` does not have; the bundled fmt has the same include
  guards as the `<fmt/core.h>` of the benchmark's `base/systems/DataSystem.h`.

Done: `emplaceOrReplace` (returns `Emplaced`, not a reference that could outlive the `making` bit; `erase` always
taking the bit instead cost +38 % in add + remove), bulk `World::destroy(span)` (`fa865dc`), `World::alive()` worked
out from the pages instead of counted (its own cost not measured).

## Resource cache

`ResourceCache<T>` in `src/sc/ecs/resource.hpp`: `bind(key)` gives a handle once, at spawn, and loads a new key on
the background lane with the cache's loader or the one given; afterwards a system only asks `ready()`. `find` and `get` read
without a lock, `forceLoad`, `apply` and `collect` change the map and must not run next to them.

- [ ] **`forceLoad` while a loader on the lane uses the old resource:** the scheduler keeps systems apart, not tasks.
  A reload that keeps the old one until the new one is loaded needs a second place in the entry.
- [ ] **Unload and reload with the handles kept:** today a resource goes only once all its handles are gone, and
  comes back by binding the key again. For entities that stay while their textures go (texture streaming, LOD), an
  entry that keeps its handles in an unloaded state, `ready()` false, and loads again when asked, without a bind.
  Only once the world streams that way; chunks that are destroyed and spawned again need nothing more.
- [ ] **One allocation per push in `DeletionQueue`:** the function with its captures, plus one per `bind` of a new key
  (the copy of the loader). Not measured; typed lists (`VkImage` + `VmaAllocation`) if thousands per frame show.
- [ ] **Frames overlapping without a render thread:** `Scheduler::run` may run in a task of the pool (2026-10-25: the
  scheduler tests pass with every run moved to a worker, also under TSan), so a render world can run its systems next
  to the simulation of the next frame (as Bevy's extract or Naughty Dog's fiber jobs do). Not tried in a frame yet:
  the run holds its worker until it is done, and the parallel stages of both worlds share the workers. Only matters
  once a CPU-bound frame shows it.

Decided on 2026-10-26, no debug check for pointers of `get`: the way to a resource is its handle (`*handle`), which
counts itself in, so collect keeps the entry, and asserts `ready()`. `get` is the shortcut for those who know how long
it lasts (until the next `forceLoad` or `collect`, in a system the end of its stage, as its comment says); it counts
nothing so that threads reading one key do not meet on a cache line, which a checked pointer would undo.

Done: `Resources<T, Key, Hash>` as a system parameter after `scheduler.provide(cache)` (finding the cache per run not
measured); GPU resources go through `memory::DeletionQueue` in their destructor, the cache knows no frames; whoever
waits for a task that has not started runs the loader itself.

- **No lock to start and finish a load:** a mutex was taken by `taskDone` on the lane, at SCHED_IDLE, and by
  `startTask` in a `bind` of a system. Now a push-only list that one thread at a time takes and dispatches from, in
  the order of `bind`. 2000 new keys bound per frame on every pool thread while the lane finishes loads of 50 us,
  median / p99 ms per frame: 0.386-0.391 / 0.62-0.70 before, 0.314-0.320 / 0.58-0.63 now; loads as fast as before.
- **Loads on the background lane** (`aa491ba`), of a loader and of a build once a read has the bytes; without
  threads in the lane, `bind` loads where it is called. maxLoads is half the lane by default, so loads leave it to
  other tasks. 500 loads bound at once next to 40 frames (Ryzen 7 5800X, 15 workers, 4 lane threads), median ms per
  frame / ms for loads and frames: without loads 1.16-1.22; sleeping 2 ms, default 1.26 / 522, whole lane 1.24-1.25 /
  266-286 (another lane task waits 2.0-2.4 ms then); computing 2 ms, default 1.29-1.34 / 566. A thread of the lane
  that waits for a resource does not join a `parallelFor` of the frame.
- **At most maxLoads loads of bind on the pool at once** (before the lane) (constructor, default a quarter of the workers when a load
  starts, at least one); the others wait in the cache, and whoever waits for one loads it itself. A loader that
  blocks keeps its worker from the frame's `parallelFor`. Measured with 500 loads that sleep 2 ms (as a blocking read
  would) bound at once, then 40 frames of a `parallelFor` of 1600 x 10 us (clang, 15 workers): no loads 1.09-1.11 ms
  median per frame; maxLoads 15 (as before) 2.94; default (3) 1.22-1.25; 1 1.09-1.15. The loads take as much longer:
  all loads + 40 frames 106 / 353 / 1 034 ms. Not measured with loaders that decode on the CPU.
- **`bind` of a key apply has taken in** looks into the map under a lock of the calling thread (64, for threadIds
  below 64; other threads take the lock of the shard), which apply, collect and forceLoad take as well. Before, the
  lock of the key's shard. ns per bind + drop of the handle, median thread, 1024 keys / one key (clang, 16 hardware
  threads, keys mixed as sc::hash gives them): 16 threads 123-128 / 1 381-1 386 before, 56-58 / 776-790 now; 8
  threads 62-69 / 391-407 before, 31-46 / 320-328 now; one thread 13.3-15.6 / 10.6-11.0 before, 13.7-14.5 /
  11.3-12.3 now. One key is now where a copy of its handle is (16 threads 833-842, 8 threads 341-347): what is left
  is the count of the entry. The writers pay: every lock of the cache without contention, measured through `size()`,
  403 ns before, 740 now; apply and collect take them at most once per stage, and only with new keys or unused
  entries.
- **A load whose handles all went before its task started is parked:** the task sees its own count as the only one
  and sets PARKED in the same word, instead of calling the loader; the next bind, find, load or forceLoad of the key
  takes the bit and starts it again (forceLoad frees it instead), collect frees it with the entry. A stress test
  (20 000 binds of 2 000 keys, four helpers) parks about 3 000 loads and starts 2 600 of them again.
- **The scheduler keeps readers and writers apart:** `Resources<const T>` reads the cache, `Resources<T>` writes it
  and runs in a stage of its own against every system that takes the cache; only `Resources<const T>` in a loop, a
  hook and a listener of an event a loop emits, as for `Global`. `run` calls `apply` of every provided cache after
  every stage, after the hooks; without a new key it takes no lock (its cost per stage not measured).
- **Bind once, as entt's `cache[id]`:** a handle has its entry from `bind` on, and the entry stays while its resource
  loads, fails or is reloaded, so nothing has to be called per frame. Tried on the way and dropped: handles that
  only knew their key and `resolve` every frame (with `request`, `apply` making the entries, a filter against 10 000
  requests of one key and a grace of one collect), binding in the handle's constructor through a global cache or a
  `thread_local` context of the scheduler (invisible, a handle without one stays empty).
- **No lock for lookups:** `find` and `get` read the map `apply` fills; keys bound since are in 64 shards with a mutex
  each, which `bind` takes. ns per call on every thread, 1024 keys / one key (clang): before, one mutex, `load` hit
  16 threads 1 640-1 790 / 1 400, one thread 11.3 / 10.1; now `get` 4-6 / 3-4, one thread 3.6 / 3.0, `find` 29-34 /
  834 (the count of the entry), one thread 9.0 / 6.2, `bind` 130 / 1 375, one thread 12.3 / 9.2. Tried on the way:
  64 shards with a `std::shared_mutex` each for every lookup (93-101 / 1 510, one thread 18.5; one key on 8 threads
  858 against 398 with one mutex: `pthread_rwlock` retries its CAS when readers meet).
- **`bind` without a loader** uses the cache's (`ResourceCache(loader)`, called with the key on the background lane);
  `load` loads a new key on the calling thread, without a task, so that no task holds a count after it returns.
- **A failed load stays** in the map, so that binding the key again does not load it again; `forceLoad` loads it anew,
  in the same entry, so every handle sees a reload.
- **`TransparentHash` by default:** an h64 key is its own hash, and one hash finds the shard and the key.
- **The CAS loop of the last handle stays:** a `fetch_sub` with a CAS only for the last one lets a bind hand out the
  entry at count 0, its handle queue it and collect free it before the first thread's CAS. Copy + drop against
  `shared_ptr`, ns: one thread 4.4 / 8.6; 1024 entries on 16 threads 24-41 / 23-36; one entry on 16 threads 760 / 380
  (the CAS retries), on 2-4 threads about the same.
- **A task that shutdown drops** fails its resource; tested with workers that return once `threadSlots()` is 1,
  which shutdown sets under the lock that requests the stop.

## Limits

Done (`4766545`): `INDEX_LIMIT` is `2^32 - PAGE_SIZE`, the index stays a `uint32_t`. Header blocks with a bitmap of
256 words (1.06 GiB of addresses per column, all promised with `vm.overcommit_memory=2`, chosen for a `findHeader`
of one load). A column made with `operator new` in a range of its own holds headers and chunks itself; a chunk table
behind a pointer had cost random `get` +21 to 27 %. Generations reserved, not a zeroed array. The test up to the
limit is replaced by `EntityAllocator(pageBits)`.

## Events and changes from systems

Goal: a system emits an event and its listeners run right away, on the emitting thread, also from `addParallel`.

- [ ] **No write per applied change:** `applyChange` sets `finish` to null in every change it does, so that `clear`
  after an exception knows what is left. A position per page (and one for the lists in order) would tell the same
  without writing 1M changes back; not measured whether it matters.
Done:

- **Mask read with `relaxed` in `parallelEach` stays** (checked 2026-10-26): under the rule below, every bit a loop
  reads and the value behind it were made before the loop started, and `parallelFor` hands them on to every thread
  that joins (the job stored with release and loaded with acquire, the ticket through `atomic_queue` the same way).
  While the loop runs, only the thread of a page changes its bits.

- **`World::modify` / `Edit::modify`** with the `making` bit, counted in the page with `occupy`: 16 threads × 2000
  rounds on 8 targets lose no update, without the bit ~19 % were lost. `Edit` and `LocalEdit` replaced `Change` and
  `Modify` (`2db7fd7`).
- **`Commands::modify`** records and runs after the stage. ns per hit (all threads unless noted), direct / recorded:
  random target, 1 thread 6.7 / 8.2; random 1.4 / 6.3; one of 1 000 targets 14.6 / 3.4; same target 15.6 / 3.8.
  Direct for few hits per entity, recorded for many. Uncontended `modify` costs ~3 ns over a write without lock;
  all threads on one target take 1.5 times a contended `fetch_add` (10.9 ns).
- **Dropped: a `Modify` that records when the bit is taken** (`tryModify`): no gain (one of 1 000 targets 14.6 →
  14.4 ns). The cost is the cache line of the making word, which every attempt brings to the core; only not
  touching it during the stage gets below one contended atomic.
- **`CommandApplier` in parallel:** changes sorted by page (counting sort per segment of 64K), one thread per page,
  erases gathered into `KillPage`s; destroys through `World::parallelDestroy`. Add + remove recorded (100K) 1.60 →
  0.54 ms, damage a random target recorded (1M) 6.30 → 3.60, destroy through `Commands` (100K) 862 → 274 µs.
- **Listeners declare their access:** `scheduler.on([](const Hit &, ...) { ... })` with the same parameters as
  systems, emitted through `Emit<Hit>`; `stages()` adds to every emitter what its listeners use. Making their
  parameters per event not measured.
- **A page emptied under its loop: not reachable.** A page empties only with the call for the last entity the loop
  visits there, and afterwards the loop reads only the change ticks, which lie apart from the values and stay; in
  `parallelEach` every page is one thread's. Tested ("World Loops Take The Looped Component From Their Own Entity",
  one spare, every emptied page pushes the one before out): passes under Wine, where reading a page pushed out
  faults (checked with a read put in on purpose).
- **`get` for writing in a parallel loop** asserted, dropped on 2026-10-22 (with `internal::mayGet` and
  `ParallelLoop`): keeping the threads apart there is the caller's to know.
- **Structural changes in a loop, the rule:** the function of a loop does not add, remove, modify or replace any
  component the loop names (also excluded ones) on other entities, also ones it creates; the own entity may lose
  the looped component. Debug builds check it with a `thread_local` `LoopWatch` and `assertNotLooped`, in `occupy`
  for adding. Three ways to make the loop see removals were measured and dropped (the mask read after every call
  +423 to +954 %; a `thread_local` count of removals +92 % on lookups and +44 % on destroy; a count per page and
  column +99 to +268 %): a full page is one loop the compiler vectorizes, and a load per entity ends that.

## `modify` inside parallel loops

Decided on 2026-10-21: the same rule as above; what other entities need goes through `Commands`, or through `Edit`
of components the loop does not name. The scheduler rejects a system that edits a component it also goes through
(`usesModifiedOtherwise`), and nested `modify` is asserted against (`internal::inModify`).

Documented on 2026-10-26: a reference a loop function gets is meant for the call, it stays on the component until
that is removed (comment on `World::each`); changes to other entities from several threads of a loop are made in no
fixed order, also through `Commands`, whose lists are applied in the order of the threads while which entities a
thread got depends on timing: only what one thread records is the same from run to run (comment on `addParallel`).

Decided on 2026-10-25: reading a component the loop writes, on other entities, through a query of the function or of
a listener of an event it emits, is the caller's to keep apart, as writing through `get` (2026-10-22); only the comment
on `addParallel` says so. Rejecting it would also reject loops whose entities do not meet those they read (the players
reading the `Health` of their enemies).

Dropped with the rule: a page lock while iterating (deadlock when two loops `modify` into each other's pages, a
waiting `modify` waits up to a whole page) and the non-blocking `modify` with a mailbox per page that needed it.

## A single-threaded variant

- [ ] **A world without thread safety:** a variant of `World` (and `Column`, `EntityAllocator`) for one thread,
  without atomics in the masks, counts and summaries, without making bits, locked instructions or `Share`s per
  thread. What it would gain is not measured; one locked instruction more per add or remove cost 25 to 45 %. Compare
  it against the parallel world with `Commands`, not only against single changes: `Commands::modify` is faster than
  a direct change where many go to the same entities (3.4 against 14.6 ns per hit, all threads on one of 1 000
  targets) because the recorded changes are applied page by page on several threads.

## Single-thread standing in ecs_benchmark

Measured 2026-10-17 on a Ryzen 7 5800X, after the per-index seed fix in ecs_benchmark (`a9f3135`). ns per entity
unless noted, rank among 7–8 frameworks.

- **Ahead:** remove + add 7.0 / 7.3 (pico_ecs 9.0, Ginseng 10.6, EnTT 20, flecs 130); `get` of one component 0.7
  (Ginseng 1.2, EnTT 2.0), two 1.3, three 1.75 (`a077ca1`); create with two components at 1024 10.2 (pico_ecs
  11.5), bulk 2.1 (flecs 4.0) since `bfd4be4`; 1 entity, 7 systems 78 ns (EnTT 70, flecs 1370).
- **Level:** dense data, `ComplexSystemsUpdate` 16K sc 265 µs, mustache 258, flecs 246, EnTT 599.
- **Behind or third:** create with two components at 262K 15.0 (pico_ecs 11.2); create empty 2.6 / 2.1 (Ginseng
  2.1 / 1.6); an empty world 4.3 µs to make and tear down (EnTT 46 ns, flecs 1.4 ms).
- `BM_AddComponent` says nothing: it pauses the timer per entity, every framework lands at ~540 ns.

- [ ] **Sparse data at medium counts, behind:** `ComplexSystemsUpdateMixedEntities` 16K: sc 175 µs, mustache 143,
  flecs 145; at 1M nearly level (14.4 / 13.8 / 13.3 ms). Exactly the even entities have the hero/monster components,
  so their masks are `0x5555…` and the loop goes bit by bit: per set bit 7 instructions of bookkeeping (`tzcnt`,
  slot, zero extension, clearing the bit) and about one branch miss per word at its end; half of every cache line is
  a gap. Raw arrays at 10 / 50 / 90 / 99 / 99.9 % density, ns per set entity: the word loop 1.23 / 0.71 / 0.63 / 0.39
  / 0.19, packed ~0.16. Tried and dropped: a linear loop with a bit test for words at least half full (+6 to +17 %),
  pointers moved on by the gap, a 64-bit slot, runs of set bits (-30 % at 99 % but +15 % at 50 %, unexplained),
  pairs of bits with a `switch`, all 64 slots with a test each. What is left is the layout, see "Entity locality".
- [ ] **Destroy, second:** 10.2 / 10.3 at 1024 / 262K, 8.2 / 9.4 without `lock` (`86d3592`); pico_ecs 6.0 / 5.5,
  all others slower (EntityX 22, EnTT 28, flecs 31). `World::destroy` looks at every column of the world: from 20
  columns on it asks each one without a virtual call whether it has the entity (`internal::ColumnPages`), below
  that the virtual calls are predicted and asking only cost. One by one, 100 types 900 → 225 ns, 24 types 82 → 56;
  a span of one entity, as one destroy through `Commands`, 100 types 1 510 → 210 ns. Through `Commands`, the whole
  `scheduler.run` per destroyed entity (all threads, 2 / 20 / 100 types): 1 000 per frame 9.8 / 9.9 / 10.0 ns, 10 per
  frame 27 / 31 / 46, 1 per frame 185 / 217 / 371, of which 143 ns is a frame whose system records nothing; 100K at
  once 6 to 13 ns, with a spread between runs not looked into. Still about 2.2 ns per column;
  a per-entity record of its columns would visit only those, at a cost in every emplace and erase. pico_ecs is
  flattered: its
  entity benchmarks never call `PicoEcsApplication::init()`, so its destroy skips `ecs_sync_destroy`. Bulk destroy
  (`fa865dc`) is below it: 4.2 / 5.3.
- [ ] **Small costs of destroy:** `valid` and `EntityAllocator::destroy` both look up `generation` and `pages()`;
  `FreeList::give` calls `resize` on every destroy, needed only if `take` took something since.
- [ ] **+3 to 5 % on unchanged paths after the parallel `giveMany`:** in `ecs_compare_sc`, destroy on one thread 820 →
  848 µs, all at once 781 → 821, add + remove recorded 541 → 564 (7 runs each), though their code did not change.
  The probe that calls `destroy(span)` directly shows no difference. Presumably code placement in that binary; not
  looked into.
- [ ] **Knee in create between 16K and 32K entities:** create with two components 10.1 → 12.3 ns per entity (14.9 at
  262K), bulk 1.6 → 3.9; pico_ecs flat at 11. It is the end of the L2 (512 KB per core): the benchmark makes and
  destroys the whole world every iteration, L2 misses per entity 0.07 at 16K, 0.31 at 32K. Not page faults, not the
  TLB. Presumably stores (write-allocate for value, mask word and count), from the counters, not shown per
  instruction. pico_ecs has half the L2 misses, partly because its entity has no generation. Unexplained, not a
  problem: small hot variables (`FreeList::left`, the `componentIndex` statics) come from L2 instead of L1 in IBS.
  Mostly a property of the benchmark; only the bytes per entity could shrink, and the generation is what buys safety.

Done: debug builds assert that `destroy`, `destroy(span)` and `parallelDestroy` run while no parallel loop and no
stage of more than one system runs on the world (`World::assertAlone`, counted with `internal::ParallelRun`); a
system that takes `World &` runs alone and may still destroy. Since 2026-10-26 also a create of a thread of the user's own
next to them, and the other way round (`World::Watch`, each side counts itself and then looks at the other), caught
where the two meet in time: a thread that creates while the main thread destroys batches of 64 asserted in 3 of 3
runs. Destroy without `lock` (`86d3592`, -10 %); bulk destroy with kill masks (`fa865dc`); `World::parallelDestroy`
with `PageSort` and `giveMany` over threads (1M entities, 6 components: 10 538 → 2 422 µs on 16 threads); bulk
create with reused indices in rows (`bfd4be4`, 2.1 instead of 10.3 ns at 1024).

## Entity locality: say at create which components an entity will get

Done as areas (`df066c0`): `world.create(components...)`, `create<Components...>()` (`08e97d5`) and
`create(span, make)` (`33027f2`) put the entity in the area of its set; area k starts at `bitreverse20(k)` and goes on
with the next free page; free lists and a `Share` per thread and area. From systems through `Spawn::create<Set...>()`
and `Edit::create(values...)`. `create(rare)` stays for entities whose components are not known at create. A throw
in a create of a span makes every entity first, then passes the exception on. Measured with `SETS_AFTER_WORLD` /
`SETS_WITH_WORLD` (100K entities in three sets, µs, after / with): the 1st set 23.8 / 5.9 on one thread, 5.4 / 3.4 on
all; memory of the world 2.36 / 1.68 MB.

- [ ] **The same sets for the other libraries** in `ecs_compare` (entt, flecs, gaia, mustache), file by file;
  archetypes iterate like the areas but move on every emplace.
- [ ] **Only a hint at create:** components added later do not move the entity. In ecs_benchmark the mixed entities
  get their hero components only after create; there it helps only if the factory gives the layout at create. The
  benchmark notes should say that sc gets a hint the others do not need (mustache's factory uses `create<...>()` the
  same way; measured neutral there, ecs_benchmark `d7e329c`).
- [ ] **Open pages per layout:** every thread keeps a partly filled page per area. Many layouts with few entities give
  many half empty pages: more memory, and `each()` touches more pages (the summary softens it). A limit, or a
  fallback group for small layouts.

## Slower after the index limit and the areas

`ecs_compare_sc`, `5e57280` (before both) against `df066c0`.

- [ ] **destroy on all threads: +33 %** (274 → 365 µs). `PageSort` now counts in one pass; in a world that destroyed
  before, the rest is level. **Open:** in a new world still slower, presumably the first use of the table of page
  numbers (`mmap`, first faults) and of the vectors per segment. A small hash table instead of the table per page
  would avoid it, at a cost per page change.
- [ ] **Looking up another entity and plain iteration, only in the benchmark binary:** lookup +5 % on one thread, +12 %
  on all; light work +8 / +15 %. A probe with the benchmark's setup shows none of it. Presumably the binary again
  (code placement), as "+3 to 5 %" above; not shown. Not covered by the probe: the parallel benchmarks go through
  `Scheduler::run` and `Query::parallelEach`, the probe calls `World::parallelEach`.
- [ ] **First use in the benchmarks:** every measured run of the create and destroy benchmarks gets a world of its own,
  for every framework alike, so they measure every first touch of its tables (create all at once +15 %, about 1.3 µs
  per page fault; `parallelDestroy` +35 µs). Not made to look better by touching the tables in the constructor.
- [ ] **create one by one in an emptied world still slower than in a new one:** 1 095 against 929 µs; `create` alone
  103 µs new, 248 emptied. Most is `FreeList::take`, one `fetch_sub` per create, where a new page needs none. Tried
  and dropped: a cache of 13 indices per `Share` (248 → 141 µs), because free indices then no longer always come
  before new pages. A game creates a few hundred per frame, where it is fractions of a microsecond.

Done: destroy all at once on one thread +24 % fixed by remembering the list of the last page (785 µs); emptied pages
no longer give their memory back at once (`MADV_FREE` on Linux, create all at once in an emptied world 539 → 174 µs;
Windows unchanged with `MEM_DECOMMIT`).

## Show the parallel advantage in numbers

ecs_benchmark is single-threaded on purpose, so none of its results shows the main argument for ecs (structural
changes inside parallel loops without deferring). Done as the churn benchmark (`bab495e`): frame at 1 / 10 / 50 %
changes, 1M, all threads: sc 0.39 / 1.53 / 6.16 ms, flecs 3.47 / 24.2 / 74.4 ms; flecs is ahead only below 0.1 to
0.3 % changes per frame. The parallel run includes a system that picks the changed entities, so compare parallel
with parallel.

- [ ] **The churn benchmark at 1, 2, 4, 8 threads; gaia.** `scripts/ecs_compare.py --threads` can run it, the
  numbers are not taken yet; `bench_gaia.cpp` has no churn case. 1M makes a full run long (flecs ~94 s, entt ~88 s, sc ~10
  s); a hidden tag is an option.
- [ ] **At 1M the systems get slower with the share of changes,** for sc and flecs alike (all threads: sc 59 → 557
  µs, flecs 44 → 424 µs from 0 to 50 %). Not the wake-up of the workers. Presumably the changes right before leave
  the data in other caches or out of them; not measured.
- [ ] **Lost updates:** many threads deal damage to few targets, the total must add up. flecs 4.0.1 in multi-threaded
  systems: `ensure`/`get_mut` hand out the component without a lock (data race), `set` is deferred and the last
  command wins at the merge, so two `hp -= 10` from `hp = 100` give 90. With `modify` it must be 80.

## Thread pool

- **Workers join late, and it does not matter much:** all 15 helpers join an `addParallel` at 100K, the last one 6.2
  µs after the start of a 13 µs loop; pages come from a shared counter, so a late helper only takes less. Spinning
  before the semaphore was tried and dropped (systems at 100K -3 µs, the frame at 50 % +3 to 5 %, CPU time 4x).
- **Scaling:** systems at 1M, no changes, 482 → 59 µs on 16 threads (8.2x; flecs 5.2x). 100K: 47 → 10 µs, about 10 µs
  being the fixed cost of two parallel loops.
- [ ] Not measured: uneven work per entity, more than 16 threads, against a pool such as TBB or OpenMP.
- **The state of the logging outlives the pool:** `initPool` calls `initLogState` before it registers
  `shutdownPool` with atexit, as `initBackground` does (`77e5e62`). Before, 64 detached tasks that throw and a return
  from `main` gave a heap-use-after-free in the registry of spdlog under TSan, 10 of 10 runs; now 0 of 10.
- **One queue, no priorities** (`f40b2df`): everything on the pool is work of the frame. The queue maps 64 MB instead
  of 3 x 64 MB of address space (commit limit on Windows).

## Background lane and Tasks

`sc::threading::spawn` and `Service` in `src/sc/threading/background.hpp`: threads beside the pool for what takes
longer than a frame or blocks; `Tasks` in `src/sc/ecs/scheduler.hpp` gives the result back through Commands at the
start of `Scheduler::run`.

- [ ] **Not measured:** the priority of the lane on Windows (`THREAD_PRIORITY_BELOW_NORMAL`); the default size of the
  lane (a quarter of the hardware threads, measured with 4 and 8); why a niceness does nothing on Linux.

Done:

- **A parameter of `addParallel` that is not a component does not compile as one:** `static_assert`s at `LoopRest`
  against `Entity` anywhere but first and against every type `SystemParam` is defined for (a system parameter after
  a component, `World &`, or one missing from `isLoopExtra`, as `Tasks` was at first). Before, it counted as a
  component and the loop ran over no entity without a word.
- **The lane** (`ef29382`): a `RingQueue` and a semaphore of its own, no lock that a system in `spawn` could wait for.
  A blocking call costs the frame nothing: 500 tasks that sleep 2 ms, median ms per frame 3.00 on the pool, 1.12-1.15
  on the lane, 1.13-1.18 without.
- **SCHED_IDLE, not a niceness** (`ef29382`): 8 tasks that compute 200 ms, median ms per frame: on the pool 2.44-2.55,
  nice 0 / 10 / 19 alike 2.50-2.71, SCHED_IDLE 1.41-1.49, none 1.13-1.18.
- **A `parallelFor` of the lane gets half the workers, only while the frame has none for them** (`ef29382`): they
  leave after the chunk in hand. A loop that runs all the time with 100 us chunks, one frame per 16 ms, median / p99
  ms per frame: no loop 1.21 / 1.3-1.4, all workers 2.2-2.6 / 4.3-4.8, half 1.20-1.27 / 1.4-2.4; the loop does 11.9
  / 6.8 s of work per second, 0.9 alone. Sixteen SCHED_IDLE threads without the pool cost the frame as much as all
  workers: the load on every core, not the wait for a chunk.
- **`Tasks`** (`0aab5d5`): `then` once at the start of `run`, so every system of a run sees the same results; a
  `TaskHandle` is one `stop_source`, `cancel` also keeps `then` from being called after the work is done. A run
  without tasks: 33.4 ns before, 35.2 with the check whether the queue is empty.

## Random access

`ecs_compare`, the 5800X, `b3e1fef` + `bab495e`: `get` of one component in random order, 100K, one thread: sc 3.9 ns,
EnTT 10.6, flecs 22.3. A system that looks up another entity, 1M: one thread sc 1.2 ns, EnTT 18.2, flecs 62.8; all
threads sc 0.19, flecs 4.6. Index → page → slot is one load per column that depends on nothing before it.
`World::get` and `Query::get` do not check in release builds whether the entity has the component (EnTT neither,
flecs does), part of the gap to flecs.

## ecs_benchmark: what its numbers say

- **Iterate\* compare code generation, not iteration:** under GCC `benchmark::DoNotOptimize(T&)` is `asm "+m,r"`, and
  GCC picks the alternative per loop; with `m` the loop only computes addresses. With a body that writes the
  components, sc 0.16–0.18 ns per entity at full pages, flecs 0.25–0.27. Until the harness has a real body (a TODO at
  `dummy_each` of the benchmark), do not quote its Iterate numbers.
- [ ] Give EnTT the scheduler of sc for the parallel benchmarks; name it "entt + sc::threading" in the report.
  Structural changes stay one-threaded there (its pools are not thread safe).
- [ ] Tags against flags: a state on 1 / 10 / 50 % of the entities as a tag (sc, flecs) and as a bool in a component
  every entity has (flecs). Expected: the loop over a rare tag is far shorter in sc, flecs pays a move per change with
  a tag and a test per entity with a flag; derived, not measured.

## Before going public

- [ ] **README:** the example still uses `Change<Frozen>` and the text explains `Modify`, both replaced by `Edit` and
  `LocalEdit` in `2db7fd7`; copied as it is, it does not compile.
- [ ] **When to use what:** `Edit`, `LocalEdit`, `Commands` in a few sentences, with the numbers of the comments
  (`Edit::modify` 6.4 ns, `LocalEdit::modify` 2.2, `Query::get` 1.1; `Commands` where many threads hit few entities).
- [ ] **Benchmarks reproducible:** hardware, compiler, flags, the command for `scripts/ecs_compare.py`; say where flecs
  is ahead (nearly static worlds, loops over randomly mixed components); show EnTT with view and with group.
- [ ] **Without meson:** list the sources of `src/meson.build`, include paths and dependencies; point to
  `src/structured-chaos/CMakeLists.txt` of ecs_benchmark as a CMake example (not maintained here); a wrap file and
  `pkg.generate()` for meson and pkg-config users.

## Tools and demos

- [ ] **Stats in the scheduler, an ImGui overview:** the stages as a graph with the reason for every edge ("A writes
  Health, B reads it"), time per system as median and 99th percentile over ~120 frames, the time of the `Commands`
  of each stage with their number; later a timeline per thread (who took which pages, where threads wait). Measured
  per system and stage, never per entity, a ring buffer per thread; behind `CHAOS_STATS_ENABLED`. Tracy zones at the
  same places give the timeline for free.
- [ ] **Ant colony as demo and torture test:** many agents that change state all the time (tags), are born and die,
  look up others (food, enemies, their colony as relation), a pheromone grid as global or resource, all through
  parallel systems and `Commands`. The same simulation with flecs and EnTT doubles as a benchmark. Start with a thin
  renderer (instancing: 1M ants are an 8 MB buffer per frame) behind a small interface; the own graphics engine can
  replace it later.
- [ ] **Saving by dumping pages:** possible because nothing moves, entity handles in components stay valid. Needs: only
  trivially copyable components (or a serializer per type), a stable id per component type (`componentIndex` depends
  on the order of first use), a version per type with migration, the state of the allocator (generations, free
  lists, areas, order of pages), resources by key, checks when loading (save files are input). Within one running
  session (rollback, replay, rewind in an editor) most of it falls away; likely much faster than for an ECS that
  reorders entities, not measured.
- [ ] **A game to try it in:** EnTT games to port: tddod (MIT, tower defense, data oriented), EnTT-Pacman (MIT),
  MonsterWar (CC0), openblack (GPL-3.0, Black & White, large and active); flecs: tower_defense of its author (MIT).
