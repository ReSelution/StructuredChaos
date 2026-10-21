# TODO

Found while integrating `sc::ecs` into [ecs_benchmark](https://github.com/abeimler/ecs_benchmark)
(`src/structured-chaos`, pinned to commit `bfd4be4`).

## Decisions of the benchmark integration to review

- **Single thread:** the `World` wrapper of the benchmark calls `sc::threading::init(0)` once, the first time a world
  is made. Without it the benchmark thread has no slot of its own in `EntityAllocator::withShare`, and every
  `create()` goes through `m_sharedMutex`. With `0` no workers run, so it stays single-threaded like the other
  frameworks.
- **No malloc override:** mimalloc is built with `MI_OVERRIDE OFF`. ecs only uses mimalloc through its own `mi_*`
  calls; the rest of the benchmark runs on the system malloc, like every other framework there. `tests/ecs_compare`
  instead uses mimalloc for all libraries.
- **fmt from vcpkg:** spdlog is built with `SPDLOG_FMT_EXTERNAL`. `SPDLOG_USE_STD_FORMAT` fails with both gcc and
  clang because `sc/logger/logger.hpp:139` uses `fmt_lib::runtime`, which `std` does not have. The bundled fmt does
  not work either: it has the same include guards as the `<fmt/core.h>` that the benchmark's
  `base/systems/DataSystem.h` includes, and both end up in the same translation units.

## Missing API in ecs

- [x] **No `emplace_or_replace`:** `Column::emplaceOrReplace` now (`World` only passes it on): one step with the
  `making` bit held from the look at the mask until the component is made or assigned; the one there gets a new
  value assigned, made from the arguments, and stays as it was if making it throws (as EnTT's). It counts itself in
  the page with `occupy` also when it replaces, since `erase` of a component without `DESTROYS` takes no `making`
  bit and could otherwise empty the page under the assignment. Returns `Emplaced::Made` or `Emplaced::Replaced`,
  not a reference, which could outlive the bit; `emplace` and `tryEmplace` return `Emplaced` as well (`Made`,
  `There`) instead of a reference and a bool. Having `erase` always take the bit instead cost +38 % in add + remove
  (`tests/ecs_compare`, 100K entities: 898 → 1238 µs) and would not have covered the reference. The benchmark's
  `addComponents` still checks with `contains` and otherwise assigns `T{}`.
- [x] **No bulk destroy:** `World::destroy(std::span<const Entity>)` since `fa865dc`; the factory of ecs_benchmark
  uses it since its commit `9bf8775`.
- [x] **No entity count:** `World::alive()` now, worked out from the pages handed out, less what the threads have
  not handed out of them yet and the free lists; create and destroy keep no count (it would be a locked instruction
  per create on a line all creating threads write). Nothing may create or destroy next to it. Its own cost (129
  `Share`s read) not measured. The benchmark tests still check with `valid()`.

## Resource cache

`ResourceCache<T>` in `src/sc/ecs/resource.hpp`: load once by key, handles with a count, given back only by
`collect(owner)` of the owner of the resource (a system at the end of the frame), loaded without waiting by
`loadAsync`.

- [ ] **One mutex for every load:** also a load that finds its resource takes it. Not measured how much threads
  that load many small resources at once wait for it. If they do, a map split by hash with a mutex each.
- [ ] **The last handle of a resource takes a CAS loop:** the count and the queued bit are one word, so that a
  handle never touches the entry after its last change (collect may free it right after). Every other drop is
  one CAS as well, not a `fetch_sub`. Not measured against `shared_ptr`.
- [x] **A parameter of systems:** `scheduler.provide(cache)` keeps a pointer to it, and systems take
  `Resources<T, Key, Hash>`, one pointer with `->` to the cache, also in the function of `addParallel`. It declares
  nothing, since everything of the cache (collect too) is safe from anywhere. One cache per type, a second `provide`
  throws; a system whose cache was not provided throws at `run`. Finding the cache is a look through the provided
  ones per run of the system, not measured.
- [x] **GPU resources:** the cache knows no frames. The destructor of a GPU resource only puts it into a
  `memory::DeletionQueue` (`src/sc/memory/deferred.hpp`), which frees it with `beginFrame` once the fence of its
  frame has signalled. Chosen over a frame delay in the cache (more state in load, find and collect, and only for
  cached objects) and over the frame holding handles (an atomic add and a CAS per resource and frame).
- [x] **Loading without waiting:** `loadAsync` runs the loader on the pool, the handle comes right away,
  `ready()` / `failed()` / `wait()`. Whoever waits for a load whose task has not started runs the loader itself, so
  that workers that all wait do not wait for a task none of them is free to run.
- [ ] **A task of loadAsync that shutdown drops:** its resource fails with an error. Not tested: dropping a task
  that has not started needs a pool whose workers are all busy at the moment of shutdown.
- [ ] **One allocation per push in `DeletionQueue`:** the function with its captures, plus one per `loadAsync`
  (the copy of the loader). Not measured; typed lists (`VkImage` + `VmaAllocation`) if thousands per frame show.
- [ ] **Frames overlapping without a render thread:** `Scheduler::run` must not be called from a pool thread, so a
  render world cannot run its systems next to the simulation of the next frame yet (as Bevy's extract or Naughty
  Dog's fiber jobs do). Only matters once a CPU-bound frame shows it.

## Limits

- [x] **`INDEX_LIMIT` beyond 2^24 (`4766545`):** now `2^32 - PAGE_SIZE`, the index stays a `uint32_t` (the last
  page is left out: the index of null lies in it, and the end of a page still fits). What grew with the limit:
  - Header blocks: a bitmap of 256 words instead of one `uint64_t`, the headers still one reservation (1.06 GiB of
    addresses per column; with `vm.overcommit_memory=2` all of it is promised, decided so for a `findHeader` of one
    load).
  - The chunk tables: first in a range of their own behind a pointer, which cost a random `get` one more dependent
    load (lookup benchmarks +21 to 27 %), and in five ranges a world of two columns 106 instead of 45 µs to make and
    drop. Now a column is made with `operator new` in a range of its own and holds headers and chunks itself, one
    load from `this` as before; create of 100K entities with 2 components in new worlds 1 541 → 1 497 µs, a world
    made and dropped 30 → 40 µs.
  - Generations: reserved, not a zeroed `std::unique_ptr` array.
  - `m_killPlace` and `PageSort` counted over all pages (4 MiB per segment): add + remove through `Commands` went
    to 19 ms. Fixed with the areas (`df066c0`), see below.
  - The test that made every entity up to the limit (4 billion, 21 GB) is replaced by `EntityAllocator(pageBits)`.

## Events with immediate delivery

Goal: a system emits an event and its listeners run right away, on the emitting thread, also from `addParallel`.

- [x] **`World::modify<Component>(entity, function)`:** calls `function` with the component while holding its
  `making` bit, counted in the page with `occupy` meanwhile (as `emplaceOrReplace`, against an `erase` without
  `DESTROYS` emptying the page); false without a call if the entity does not have the component. For systems the
  parameter `Modify<T...>`, which declares writing `T` and takes the component from the function's parameter:
  `modify(target, [](Health &h) { ... })`. 16 threads × 2000 rounds on 8 targets lose no update; without the bit
  about 19 % were lost (26 000 instead of 32 000). Loops and `get` still do not take the bit, see below.
- [x] **`get` for writing in a parallel loop asserts:** `World::get` and `Query::get` with a non-const component
  assert in debug builds while the thread runs the function of `parallelEach` (`internal::inParallelLoop`, set per
  chunk, also with no helper thread). The scheduler keeps other systems away, not the other threads of the same
  loop; reading with `get<const T>` stays allowed. Writing another entity's component there is for `modify`.
  `World::column<T>().get` gets past it.
- [x] **`Commands::modify(entity, function)`:** the function is moved into the thread's `CommandList` and called after
  the stage, in the order recorded with the thread's `emplace`/`erase`, without the bit (nothing runs then); does not
  count as writing the component, so readers of it run in the same stage and see the old value. Same benchmark as
  `modify` (ns per entity, all threads unless noted):

  | | `Modify` (now) | `Commands::modify` (after the stage) |
  |---|---|---|
  | random target, 1 thread | 6.7 | 8.2 |
  | random target | 1.4 | 6.3 |
  | one of 1 000 targets | 14.6 | 3.4 |
  | same target | 15.6 | 3.8 |

  Few threads per target: `Modify`, which runs where the threads are; recording only pays off where they would
  wait for each other. Applying is one thread for all lists.
- [x] **Tried and dropped: `Modify` that records instead of waiting.** `Column::tryModify` took the bit once; if
  another thread had it, the function went to the thread's `CommandList` for after the stage (`Modify` then had
  the system's lists). Meant to bring the contended cases down to `Commands::modify`; it did not (all threads, ns
  per hit, waiting / record on a failed `fetch_or` / record on a set bit seen by a plain load first):

  | | waiting | `fetch_or` once | load first |
  |---|---|---|---|
  | random target | 1.41 | 1.64 | 1.85 |
  | one of 1 000 targets | 14.6 | 14.4 | 17.2 |
  | same target | 15.6 | 16.6 | 13.0 |

  The cost is not the waiting but the cache line of the making word: every attempt brings it to the core, a
  failed one as well, and every thread that gets the bit writes it twice, which takes it away from all readers
  again. Reading first saves the exclusive fetch only while the bit stays set (same target) and adds a second
  transfer where it is mostly free. A making word covers 64 entities, so with one of 1 000 targets the threads
  also fight over 16 words for different entities. Anything that asks at the target during the stage stays at
  about one contended atomic (10.8 ns); only not touching it, `Commands::modify`, gets below. Kept apart instead:
  `Modify` for few hits per entity, `Commands::modify` for many, as their comments say.
- [x] **Apply recorded changes in parallel (`CommandApplier`):** after a stage the changes of all lists are sorted by
  the page of their entity (counting sort, stable, counted and placed per segment of 64K changes on several
  threads), and every page goes to one thread: the order of an entity's changes stays, and no other thread touches
  the page in any column. So the erases of a page are gathered per column into a `KillPage` and done with
  `eraseAlone`, a mask word for up to 64 entities, flushed before any emplace or modify. `eraseAlone(span)` now
  lowers the count with `forsake`, one locked instruction per page, as other threads change other pages and a page
  leaving the spares may look at its count. The entities to destroy go to `World::destroy(span)` at once, after all
  changes of all lists (before: after the changes of their own list). When the lists noted while recording that all
  their changes are in one page, there is no sorting at all. Before and after, ms, all threads unless noted:

  | | before | after |
  |---|---|---|
  | add + remove a component, recorded (100K) | 1.60 | 0.54 |
  | destroy entities through `Commands` (100K) | 1.01 | 0.87 |
  | damage a random target, recorded (1M) | 6.30 | 3.60 |
  | the same, recorded on one thread (1M) | 8.10 | 6.43 |
  | one of 1 000 targets, recorded (1M) | 3.45 | 3.24 |
  | the same target, recorded (1M) | 3.77 | 3.37 |

  On the way: sorting on one thread made the one-page cases worse (6.2 instead of 3.4 ms, about 2.8 ms for 1M
  changes); per list was not enough when one list had them all; and `clear` went through all changes once more to
  drop values (32 MB for 1M), now only after an exception.
- [x] **Destroys of a stage over threads:** `CommandApplier` calls `World::parallelDestroy`, see "Bulk destroy over
  threads". Destroy through `Commands`, 100K entities, all threads: 862 → 339 µs.
- [ ] **No write per applied change:** `applyChange` sets `finish` to null in every change it does, so that `clear`
  after an exception knows what is left. A position per page (and one for the lists in order) would tell the same
  without writing 1M changes back; not measured whether it matters.
- [x] **Listeners declare their access:** `scheduler.on([](const Hit &, Modify<Health>, Commands) { ... })`, the
  event as the first parameter and after it the same `SystemParam`s as systems; a system emits with an `Emit<Hit>`
  parameter (also in `addParallel`), which calls the listeners right away on its thread. `stages()` adds to every
  system what the listeners of the events it emits use, of the events they emit as well, so listeners may be added
  after the systems. A listener with nothing but the event makes its emitters run alone, as a system without
  parameters. What `static_assert` checks for one system (a component taken through `Modify` and past it) is checked
  there over the system and its listeners, with `std::logic_error`. The parameters of a listener are made for every
  event, `Resources` with its look through the provided caches; not measured.
- [x] **Adding and removing in an `each`:** what happens when the function of an `each` or `parallelEach` adds or
  removes components, through `Change`, `World` directly, or a listener of `Emit`. By design a structural change has
  no side effects on other components (one column per component, values never move), so only the column the loop
  goes through matters. Tried with a probe under ASan (`each<Owned>`, `Owned` holding a `unique_ptr`), single thread:

  | change in the function | what the loop does |
  |---|---|
  | own entity, a component the loop does not name | nothing, other column |
  | own entity, erase of the component the loop goes through | goes on, 100 of 100 and 4096 of 4096 calls; only the function's reference dies |
  | other entity, erase, later word of the page | skipped: the mask of a word is read when the loop gets to it |
  | other entity, erase, same word, later slot | **called with the destroyed component** (the mask of the word was read before) |
  | other entity, erase, full page | **called with the destroyed component**: a full page is gone through without the mask |
  | other entity, add, later word already in the summary | seen |
  | other entity, add, later word not in the summary | not seen: the summary is read once per page |
  | other entity, add, same word, later slot | not seen |
  | other entity, later page | seen either way: the page is read when the loop gets to it |

  So the documented rule (only the own entity, not the components the loop names) is what keeps it safe; nothing
  checks it. Not tried, from reading: an erase that empties the page the loop is in parks it among the spares, which
  keep its memory; only if other pages emptied meanwhile push it out does `vacate` give the memory back
  (`memory::decommit`) under the loop, which reads zeros on Linux and faults on Windows. In `parallelEach` the
  loop reads the mask with `relaxed` while `tryEmplace` of another thread sets it with `acq_rel`: seeing the bit
  does not formally guarantee seeing the made value (fine on x86, not on ARM).

  Three ways for the loop to see what the function removed on the same thread, tried and measured
  (`scripts/ecs_compare.py --only sc`, best of 2 runs each against 4 runs as now; the same build differs by about
  ±10 % in rows nothing changed, `get` among them):

  | | 1 component, 1 thread | 3 components | 5 systems | looks up another entity | add + remove | destroy, all at once |
  |---|---|---|---|---|---|---|
  | as now | 22.3 µs | 13.8 µs | 952 µs | 1.26 ms | 780 µs | 463 µs |
  | the mask read after every call | +423 % | +954 % | +485 % | +89 % | +1 % | +3 % |
  | a `thread_local` count of removals, the mask read anew when it went up | +2 % | +1 % | −2 % | +92 % | +5 % | +44 % |
  | a count per page and column (`Header::changes`), the same | +99 % | +268 % | +251 % | +246 % | +45 % | +33 % |

  A full page is one loop the compiler turns into SIMD code; a load of the mask or of an atomic count per entity
  ends that. The `thread_local` count can be hoisted where the function is seen through, but not where it calls
  what the compiler does not see (looking up another entity, `modify`), and why removing got 38 to 44 % slower
  with it was not found. The count per page sees other threads too, but does not stop them: a component removed
  between the look and the call is still called destroyed.

  Decided: none of them. The rule stays as documented (only the own entity, not the components the loop names; for
  others `Commands`), and debug builds check it: a loop puts its columns and the entity of its call into a
  `thread_local` `LoopWatch`, and `erase` and `eraseAlone` assert that they take no component of a watched column
  from another entity (`assertNotLooped`), also through nested loops and listeners of `Emit`. Removing the looped
  component of its own entity stays allowed: the loop is fine with it, only the function's reference is gone.
  Nothing in release builds (one run: 62 of 67 rows within the range of the runs before ±3 %, the rest in both
  directions). Adding to another entity is not checked: the loop may or may not see it, but nothing is destroyed.
  Changes by other threads are not caught either; for them a loop that holds the making bit would be needed.
- [x] **Measure:** cost of `modify` per event, and with many threads waiting on the same target (one boss, many
  hits), as a case in `tests/ecs_compare` (large world, 1M entities, each damages another by 1; Ryzen 7 5800X,
  16 threads, clang release, median of 5 runs, ns per entity):

  | | 1 thread | all threads |
  |---|---|---|
  | random target, `get` without lock | 3.7 | – |
  | random target, `modify` | 6.7 | 1.4 |
  | one of 1 000 targets, `modify` | – | 14.5 |
  | same target, `modify` | 3.8 | 16.0 |
  | same `std::atomic`, `fetch_add` (what the hardware takes at least) | – | 10.9 |

  Uncontended `modify` costs about 3 ns over a write without lock (two locked instructions). With all threads on
  one target it takes 1.5 times a single contended `fetch_add`. The first version also counted itself in the page
  with `occupy`/`forsake`: 10.8, 2.1, 37.8, 7.0 and 22.2 ns. One of 1 000 targets was the worst case then, since
  all of them lie in page 0 and every thread wrote its count. Now `vacate` keeps a page whose making bits are
  held instead (`Column::heldHas`), and add + remove is unchanged (899 to 909 µs against 898).

## `modify` inside parallel loops: directions

Decided on 2026-10-21: the function of a loop does not touch the components the loop goes through on other
entities, the same rule as for adding and removing (see "Adding and removing in an `each`"). What other entities
need goes through `Commands`, or through `Modify` of components the loop does not name.

- [x] **Same-loop race:** `modify` only helps if every writer takes the lock. A `parallelEach<Health>` writes its own
  pages without it; if its callback calls `modify<Health>` on an entity in a page another thread is iterating, both
  write the same `Health`. For systems a `static_assert` in `Scheduler::addSystem` now forbids `Modify<T>` together
  with `T` as a loop parameter or in a query (also `const T`: reading without the bit next to a `modify` is a race
  as well). For `world.parallelEach` with `world.modify` in it, which has no types to check, debug builds assert
  in `Column::modify` that it is not a component the loop goes through on another entity (`assertNotLooped`).
- [x] **Page lock while iterating: dropped,** see the decision above. It would only have let a loop `modify` the
  components it goes through. As it was planned: a loop locks the page of each column it writes (per column and page, not per
  entity page, so a loop over `Health` does not block `modify<Sprite>`), one RMW per 4096 entities. Pitfalls:
  - Deadlock: thread A holds page P1 and calls `modify` on P2, thread B holds P2 and calls `modify` on P1 (two groups
    hitting each other). `modify` must never block while its thread holds a page lock.
  - Latency: a waiting `modify` waits up to a whole page; many threads hitting one target (the player's page) all
    wait for one thread.
- [x] **Non-blocking `modify`: dropped** with the page lock it needs. As it was planned: the page lock holds the
  owning thread id.
  1. page not locked: run directly with the `making` bit;
  2. locked by this thread: run directly, same thread writes in order;
  3. locked by another thread: push the callback into a lock-free mailbox of that page; the owner drains it before
     unlocking.
  No deadlock, no lost update, the change still lands in the same loop. Costs: a per-thread arena for the callbacks
  (no `std::function` on the heap), mailbox order is not deterministic (the read-modify-write result is still right).
  Only worth it if `modify<T>` inside loops over `T` is common; otherwise the assert is enough.
- [x] **Nested `modify`:** a callback that calls `modify` on another entity can deadlock (A holds e1 wants e2, B holds
  e2 wants e1). Asserted in debug builds (`internal::inModify`).
- [ ] **Escaping references:** C++ cannot stop a callback from storing `&component`; document it. Reads need no lock:
  `get<const T>` on other entities is already safe within a stage, the scheduler keeps writers of `T` out of it and
  `Commands::erase` only applies at the end of the stage.
- [ ] **Determinism:** immediate changes make it timing dependent whether another loop sees them (see the comment on
  `World::each`); a command buffer applies them in a fixed order. Relevant for replays and lockstep networking; state
  it as a trade-off, not only as a speed gain.

## A single-threaded variant

- [ ] **A world without thread safety:** a variant of `World` (and `Column`, `EntityAllocator`) for one thread,
  without what makes the world safe for many: no atomics in the masks, counts and summaries, no making bits, no
  locked instructions in emplace, erase and modify, no `Share`s per thread in the allocator. What it would gain is
  not measured; in the measurements above, one locked instruction more per add or remove cost 25 to 45 %. Worth
  knowing first: `Commands` is faster than a direct change where many go to the same entities (`Commands::modify`
  3.4 against 14.6 ns per hit with all threads on one of 1 000 targets) because the recorded changes are applied on
  several threads, page by page. A single-threaded world loses that, so compare it against the parallel one with
  `Commands`, not only against single changes.

## Single-thread standing in ecs_benchmark

Measured 2026-10-17 on a Ryzen 7 5800X, after the per-index seed fix in ecs_benchmark (`a9f3135`; before it the
hero/monster data differed per framework). Small counts with the page summary (`872ede0`).

- **Structural changes, ahead:** remove + add of one component (`BM_RemoveAddComponent`, 1024 entities): sc 7.1 µs,
  Ginseng 12 µs, EnTT 20 µs, flecs 133 µs. A bit and a construction in place, no table move (archetypes) and no
  swap-and-pop (EnTT).
- **Small counts, ahead:** 1 entity, 7 systems: sc 78 ns, EnTT 70 ns, mustache 127 ns, flecs 1370 ns (395 ns before
  the summary).
- **Dense data, level:** `ComplexSystemsUpdate` 16K: sc 265 µs, mustache 258 µs, flecs 246 µs, EnTT 599 µs; 262K:
  4.51 / 4.50 / 4.39 ms.
- [ ] **Sparse data at medium counts, behind:** `ComplexSystemsUpdateMixedEntities` 16K: sc 175 µs, mustache 143 µs,
  flecs 145 µs (about 20 %); at 1M nearly level (14.4 / 13.8 / 13.3 ms). Exactly the even entities have the
  hero/monster components, so their masks are `0x5555…` and the loop goes bit by bit. Measured per frame (16K) against
  flecs: +350k cycles, +500k instructions (the `tzcnt` loop and the address of every component), +18k L1 misses
  (half of every cache line of Health/Damage/Sprite/Player is a gap), +3k–6k branch misses. The extra misses are in
  the `if`s of the benchmark's Health/Damage logic, not in the loop of ecs, and they are not the bottleneck: with
  clang 3k fewer, same time. Tried without gain: a linear loop with a bit test for words at least half full (+6 to
  +17 %), pointers moved on by the gap instead of `values + slot` (+6 % instructions, same cycles). What is left is
  the layout; see "Entity locality" below.

Entity benchmarks, ns per entity, rank among 7–8 frameworks (mustache has no destroy/remove-add results). Create and
destroy from the reworked benchmarks of ecs_benchmark (`c00a2d1`: one registry for all iterations, so indices are
reused; 1, 64, 1024 and 262K entities, 0.3 s each), the rest from the full run. Before the rework every create and
destroy result held the teardown of a registry per iteration (sc::ecs 15 µs, flecs 470 µs), which made sc look
twice as slow at 1024 as it is.

- **`get`, ahead:** one component 0.7 at 1024 and 262K (Ginseng 1.2, EnTT 2.0, flecs 10.7). Two components 1.3,
  three 1.75 (Ginseng 2.1, pico_ecs 2.3, EntityX 2.5, gaia 2.7) since `componentIndex` inlines (`a077ca1`; before
  2.4 and 2.85, rank 5).
- **Remove + add, ahead:** 7.0 / 7.3 (pico_ecs 9.0, Ginseng 10.6, EnTT 20, flecs 130).
- **Create with two components, ahead at 1024:** 10.2 (pico_ecs 11.5, Ginseng 12.6, EnTT 18.4, mustache 113, flecs
  112). At 262K third: 15.0 (pico_ecs 11.2, Ginseng 12.4, EnTT 18.4).
- **Create empty, third:** 2.6 / 2.1 at 1024 / 262K (Ginseng 2.1 / 1.6, pico_ecs 2.4 / 1.9, EnTT 6.8 / 9.8).
- **An empty world:** `BM_CreateNoEntities` 4.3 µs for making and tearing it down (Ginseng 1.7 ns, EnTT 46 ns,
  mustache 0.7 µs, gaia 8.4 µs, flecs 1.4 ms).
- [ ] **Destroy, second:** 10.2 / 10.3 at 1024 / 262K, 8.2 / 9.4 without `lock` (`86d3592`); pico_ecs 6.0 / 5.5,
  all others slower (EntityX 22, EnTT 28, flecs 31). pico_ecs only tests a bitset that lies with the entity and leaves the components
  where they are. `World::destroy` calls `erase` through a virtual call on every column of the world, not only on
  those the entity has, each with `findHeader` and a mask load; for a component the entity has, a `fetch_and` and a
  `fetch_sub` (seq_cst) on top (gone since `86d3592`), then `vector::resize` and `push_back` in `FreeList::give`. A game with 100 component
  types makes 100 virtual calls per destroy. A per-entity record of its columns would let `eraseAll` visit only
  those. pico_ecs is flattered by the benchmark: its
  systems are registered in `PicoEcsApplication::init()`, which the entity benchmarks never call, so its destroy
  skips `ecs_sync_destroy` (removing the entity from the sparse set of every system it is in).
- [x] **Destroy without `lock` (`86d3592`):** `destroy` never runs next to a system (`Commands::destroy` only
  records, `CommandList::apply` runs alone), so `World::destroy` now takes the components with
  `Column::eraseAlone`: relaxed load and store instead of `fetch_and`/`fetch_sub`, no `making` bit, the summary set
  to 0 without a rebuild. 10 % less, not the hoped jump to pico_ecs: perf put about a quarter of the samples on the
  locked instructions and right after them, but the plain loads and stores remain. The rest is spread thin over
  the loop over the columns (devirtualized `erase` with its type check), `valid`, the generation and
  `FreeList::give`.
- [ ] **Small costs of destroy:** `valid` and `EntityAllocator::destroy` both look up `generation` and `pages()`;
  `FreeList::give` calls `resize` on every destroy, needed only if `take` took something since.
- [x] **Bulk destroy with kill masks (`fa865dc`):** `World::destroy(span)` sets a bit per entity in a kill mask of
  its page (`EntityAllocator::destroyMany`: `valid`, so duplicates and gone ones drop out, `++generation`, the
  indices back to the free lists in one step each); then per column one call, `Column::eraseAlone(span<KillPage>)`:
  per page one header, per word one change of the mask for up to 64 entities, the count once per page, the summary
  bit of a word that became empty cleared. No `lock`. 4.2 instead of 8.4 ns per entity at 1024, 5.3 instead of 9.2
  at 262K (against a loop over `destroy`); below the single destroy of pico_ecs (6.0 / 5.5).
- [x] **Bulk destroy over threads (`World::parallelDestroy`):** not the second step, as guessed, but mostly the
  first: on one thread ending the entities (`valid`, generation, free list, kill bit) took 6.6 ns per entity without
  components, and every column only 0.4 to 0.6 ns more. So the entities are sorted by page (`PageSort`, taken out of
  `CommandApplier`), and every page goes to one thread, which ends its entities (`EntityAllocator::kill`) and takes
  their components in every column (`eraseAlone` of its `KillPage`); only `giveMany` to the free lists is left to
  the calling thread. A name of its own, as `each`/`parallelEach`: `destroy(span)` stays on the calling thread.
  `destroy` / `parallelDestroy`, µs, 16 threads: 100K entities without components 699 / 254, with 6 components
  952 / 353; 1M 6 745 / 1 644 and 10 538 / 3 060. The free lists get the indices in page order, not in the order
  given.
- [x] **`giveMany` over threads too:** split up by measurement (1M entities, 6 components, µs): counting 135,
  placing 340 to 770, the work on the pages 1 300 to 2 000, compacting the freed indices 1 (with no entity given
  twice they already lie where they go, and `memmove` onto itself is skipped), `giveMany` 905 to 983. So only
  `giveMany` was worth it: the free lists get their final size first and every page copies its indices to its
  place (`EntityAllocator::giveMany` of pages), which also replaces the compacting. `parallelDestroy`, µs, before
  / after: 100K without components 250 / 184, with 6 components 343 / 303; 1M 1 680 / 963 and 3 167 / 2 422.
  Destroy through `Commands`, 100K: 338 → 274.
- [ ] **+3 to 5 % on unchanged paths after the parallel `giveMany`:** in `ecs_compare_sc`, destroy on one thread
  820 → 848 µs, all at once 781 → 821, add + remove recorded 541 → 564 (7 runs each), though their code did not
  change. The probe that calls `destroy(span)` directly shows no difference (100K 691 / 690 / 700 against 687 /
  702 / 689). Presumably code placement in that binary (new instantiations, other inlining); not looked into.
- [ ] **`World::destroy` must not run next to other threads, assert it:** the comment says so since `86d3592`
  (like `CommandList::apply`). Before, nothing said so and `eraseAll` even said "Safe next to threads that keep to
  other entities", while `FreeList::give` (`resize`, `push_back`) runs against `take` from `create` without a lock:
  thread A in `create` reads `indices[before - 1]` while thread B in `destroy` pushes back, the vector reallocates,
  A reads freed memory. Now also the components go without locked instructions. Left: a debug assert if ecs can
  tell that a loop or stage is running.
- [x] **Bulk create only fast with fresh indices (`bfd4be4`):** every reused index went to the columns on its own
  (`run(index, 1)`: count, mask and summary per entity). `FreeList::takeMany` takes them with one `fetch_sub`, and
  `create(span)` hands every row of them that follow one another to the columns in one call. With two components
  2.1 instead of 10.3 ns per entity at 1024 (flecs 4.0, EnTT 12.5), 6.3 instead of 14.5 at 262K (flecs 3.5, EnTT
  13.0).
- [ ] **Knee in create between 16K and 32K entities:** create with two components 10.1 → 12.3 ns per entity (13.4
  at 65K, 14.9 at 262K), bulk create 1.6 → 3.9 (5.0, 6.4; flecs 3.3 to 4.7), create without components flat at 2.1;
  pico_ecs flat at 11. Measured: it is the end of the L2 (512 KB per core). The benchmark makes and (paused)
  destroys the whole world every iteration; up to 16K all that sc touches fits in L2, from 32K not. L2 misses per
  entity 0.07 at 16K, 0.31 at 32K (×~40 cycles of L3 ≈ the 2 ns); L1 misses per entity only 1.1 → 1.75. Not page
  faults (2.7 per iteration at 65K: `offer` leaves the memory where it is) and not the TLB (dTLB misses rise, but
  ~0.4 µs per iteration). That disproves the earlier guess of memory given back and faulted in again.
  - IBS (`perf mem`) sees almost no loads from L3 (43 of 136k samples), so the extra L2 misses are presumably stores:
    write-allocate for the value, mask word and count per create, and mask/count again in destroy. IBS gives stores
    no source; a conclusion from the counters, not shown per instruction.
  - pico_ecs has about half the L2 misses per entity. Part of it: its entity is an id without a generation, so no
    array of 4 bytes per entity that create and destroy touch (and no detection of stale handles).
  - Unexplained: small hot variables come from L2 instead of L1 in IBS: `FreeList::left` 95 % of its loads (read and
    `lock xadd` on every create), the `componentIndex` statics of Position/Velocity 60–78 %, `started` in the page
    header 52 %. `left` alone is about one L1 miss per entity, about the whole L1 miss rate. Maybe set conflicts in
    L1 (the big arrays at addresses with the same offset), maybe how IBS reports atomics. Not a problem, not
    understood.
  - Mostly a property of the benchmark (the whole world churned per iteration); a game that makes a few hundred
    entities per frame works elsewhere. No fault in ecs seen; only the bytes per entity could shrink, and the
    generation is the part that buys safety.
- `BM_AddComponent` says nothing: it pauses the timer per entity, every framework lands at ~540 ns.

## Entity locality: say at create which components an entity will get

Done as areas (`df066c0`): `world.create(components...)` and `create(span, components...)` put the entity in the area
of its set; area k starts at `bitreverse20(k)` (van der Corput: 0, 2^19, 2^18, 3 * 2^18, ...) and goes on with the
next free page; free lists and thread shares per area. The original idea: Entities with the same set of components get their indices from the same pages, so their masks
are full and their components lie without gaps: the full-page/full-word loops apply and every loaded cache line is
used. Archetype locality at create, without the move on every later change. `create(rare)` is the small version of
it already (own pages at the top of the index space).

- [x] **API:** `world.create(Position{}, Velocity{}, Health{})`; the set is numbered once per type list
  (`setIndex`, a static), the area per world in a table: one load per create, no handle needed.
- [ ] **Only a hint at create:** components added later do not move the entity, the index stays. In ecs_benchmark the
  mixed entities get their hero components only after create; there it helps only if the factory gives the layout
  at create. Legitimate use of the API, but the benchmark notes should say that sc gets a hint the others do not
  need.
- [ ] **Open pages per layout:** still open with areas: every thread keeps a partly filled page per area. Every layout keeps its own partly filled page. Many layouts with few entities give
  many half empty pages: more memory, and `each()` touches more pages (the summary softens it, the page loop and
  `page()` calls stay). A limit, or a fallback group for small layouts.
- [x] **`create<Components...>()` (`08e97d5`):** picks the area by the types and makes no component; they come
  with `emplace` afterwards. `create(components...)` stays for an entity that gets its values right away.
- [x] **From systems (`9eb110b`):** `Change::create<Set...>()`, `Change::create(values...)` with named components and
  `Commands::create<Set...>()`, all right away, nothing recorded. Before, everything a system spawned went to the
  common area.
- [x] **Many with values of their own (`33027f2`):** `create<Components...>(span, make)`, `make(i)` gives a tuple,
  moved into place a page at a time. 100K with two components in a new world: 149 µs, against 888 with an
  `emplace` each and 113 for copies of the same values.
- [x] **A throw in a create of a span:** `EntityAllocator::create` stopped there, handed the new indices of that
  run out once more and lost the reused ones after it (also `create(span, values...)` when a copy threw). Now
  every entity is made, the exception goes on afterwards.
- [x] **`create(rare)` next to the areas:** rare is a fixed area (1) without a set. With sets it is mostly not
  needed (`create<Player>()` keeps players together); it stays for entities whose components are not known at
  create. The documentation of `Rare` says so now.
- [x] **Reused indices:** a free list per area; a page knows its area in 16 bits. Was: one free list for common and
  one for rare entities. For a destroyed hero to make room
  for a hero again, every layout needs its own free list; otherwise the index space mixes again over time (the state
  the reworked create benchmark of ecs_benchmark measures, with reused indices).
- [x] **Threads:** a `Share` per thread and area (129 per area, 8 KiB). Pages are handed out under a lock, once per
  4096 entities, so that the row of pages the loops go through has no gap.
- [x] **Measure the benefit:** `SETS_AFTER_WORLD` / `SETS_WITH_WORLD` in `ecs_compare` (sc only so far): 100K entities
  in three sets created in turn (Position + Velocity, Position + Health, Position), given their components after
  `create` against created with them. µs, 5 runs, after / with: the 1st set 23.8 / 5.9 on one thread (a third of
  the entities at the speed of full pages), 5.4 / 3.4 on all; the 2nd set 20.1 / 2.6 and 4.8 / 2.9; Position, which
  all have, 22.8 / 23.6 and 5.3 / 5.3. Memory of the world 2.36 / 1.68 MB. Next: the same sets for the other
  libraries, file by file; archetypes iterate like the areas but move on every emplace. Was:
- [ ] **Measure the upper bound first:** a variant of the mixed update in which the hero entities get one contiguous
  index range (e.g. with `create(rare)` or bulk create). It shows how close sc gets to mustache/flecs with perfect
  locality; only if that pays off does the API.

## Slower after the index limit and the areas

`ecs_compare_sc`, 5 runs alternating, `5e57280` (before both) against `df066c0`. The benchmarks create their
worlds with `create(span, Position, Velocity)`, so those entities now lie in an area at page 2^18. The rest is
level or faster (random `get` -4 %, damage without lock -34 %, add + remove through `Commands` -15 %).

- [x] **destroy all at once, one thread: +24 %** (823 → 1 020 µs). perf: `destroyMany` took twice as long, the chain
  owner → table of areas → end of the list per entity. `destroyMany` and `giveMany` of indices remember the list of
  the last page: 785 µs (800 before both steps), probe of `destroy(span)`, 100K entities.
- [ ] **destroy on all threads: +33 %** (274 → 365 µs). Split up by phase (µs, 100K entities): `PageSort` counted
  every item twice and looked its page up every time; now one pass that counts, the number of the last page kept,
  per segment a list of the pages it numbered. In a world that destroyed before: sorting 42 / 55, placing 61 / 51,
  the rest level. **Open:** in a new world still 42 / 64 and 79 / 92, presumably the first use of the table of
  page numbers (`mmap`, first faults) and of the vectors per segment; the benchmark makes a new world per sample,
  a game uses one. A small hash table instead of the table per page would avoid it, at a cost per page change.
- [ ] **Looking up another entity and plain iteration, only in the benchmark binary:** 9 runs alternating, `5e57280`
  / `6825041`: lookup +5 % on one thread (997 → 1 047 µs), +12 % on all (171 → 191); light work +8 % (193 → 208),
  +15 % on all (44 → 51). Not `valid` (the benchmark's `Query::get` does not call it) and not the areas (the
  benchmark creates with `create()`, area 0). A probe with the benchmark's setup (1M entities one by one, six
  components, targets in random order) shows none of it: `each` 168–171 / 166–167 µs, `parallelEach` 35–37 / 35–36,
  lookup 2 832–2 850 / 2 673–2 705 (-5 %), in area 0 and in an area at page 2^18 alike. The serial light work is the
  same `World::each` in both. Presumably the binary again (code placement, or what ran before in the same process),
  as the open "+3 to 5 %" entry above; not shown. Not covered by the probe: the parallel benchmarks go through
  `Scheduler::run` and `Query::parallelEach`, the probe calls `World::parallelEach`.
- [x] **create all at once: +15 %** (127 → 146 µs): first use, not slower code. A probe like the benchmark (a new
  world per run, made outside the timing, the memory of the one before back in `rangeCache`): 95.7 µs at `5e57280`,
  102.8 at `4766545`, 115.8 now, with 15 / 21 / 30 page faults inside `create(span)`, about 1.3 µs per fault. Every
  table that gets memory with its first use costs a fault there in a new world: the chunk table of each column (a
  zeroed `std::array` before, touched in the constructor), the owners, the row of pages, the table of areas, the
  `Area` itself, the areas of the sets in the world; with the areas their entries lie at page 2^18, on pages of
  their own. A game pays it once per world and area (~20 µs); see below for the benchmarks.
- [ ] **First use in the benchmarks:** every measured run of the create and destroy benchmarks gets a world of its
  own (`make_worlds`, made outside the timing), for every framework alike. They measure the first fill and the
  first destroy of a world, with every first touch of its tables: create all at once +15 % (above),
  `parallelDestroy` +35 µs (above, the table of page numbers of `PageSort`). Not made to look better by touching
  the tables in the constructor, which the benchmarks do not time. The variants in a world emptied before
  (`CREATE_AGAIN`, `CREATE_BATCH_AGAIN`, so far only for sc) were meant to show the other case and showed
  something else, see the next one.
- [x] **Emptied pages gave their memory back at once:** a world filled, emptied with `destroy` and filled again took
  5 times as long as a new one (522 instead of 95 µs, 336 page faults: `vacate` gave every emptied page but the 4
  spares back with `MADV_DONTNEED`, and every piece came back zeroed). Since `5e57280` at least, not a regression.
  `memory::decommit` uses `MADV_FREE` on Linux now (the memory stays until the OS needs it, as with `offer`), and
  `startPage` no longer populates a page that gave its memory back. `ecs_compare_sc`, 5 runs: create in an
  emptied world 1 512 → 1 095 µs, all at once 539 → 174; destroy 852 → 761, all at once 752 → 674; add + remove
  904 → 795, on all threads 142 → 118; create in a new world level. Memory: the emptied pages count as Rss
  (`LazyFree`) until taken, the world after 2 200 frames 4.89 → 4.95 MB. Windows unchanged (`MEM_DECOMMIT`).
- [ ] **create one by one in an emptied world still slower than in a new one:** 1 095 against 929 µs. Split up
  (probe, 100K): `create` alone 103 µs in a new world, 248 in an emptied one; the two `emplace`s level (832 / 790).
  The falling order of the indices is little of it (237 when emptied backwards, so that they come out rising). Most
  is `FreeList::take`, one `fetch_sub` per create, where a new page needs none (`share.next++`).
  - Tried and dropped: every thread takes `Share::CACHED` (13, what fits the cache line of its `Share`) indices
    with one `fetch_sub` and hands them out from there: 248 → 141 µs, a new world level. But a thread whose cache
    and the free list are empty takes a new page while others still hold up to 12 each, so free indices no longer
    always come before new pages (the scheduler test "A Loop Changes Its Entity Next To Entities Created In The
    Same Page": 4 pages instead of 3). Kept the rule; a game creates a few hundred per frame, where it is fractions
    of a microsecond.

## Show the parallel advantage in numbers

ecs_benchmark is single-threaded on purpose, so none of its results shows the main argument for ecs (structural
changes inside parallel loops without deferring).

- [ ] **Structural changes in a parallel loop:** per frame 1 %, 10 %, 50 % of the entities gain or lose a component
  inside a system that runs over all entities in parallel. sc: directly in `parallelEach`; flecs/gaia: deferred mode
  or command buffer plus flush; EnTT: collect and apply afterwards (no parallel `each`). Time per frame including the
  flush, at 1, 2, 4, 8 threads.
- [ ] **Lost updates:** many threads deal damage to few targets, the total must add up. flecs 4.0.1 in multi-threaded
  systems: `ensure`/`get_mut` hand out the existing component without a lock (data race), `set` is deferred
  (`flecs_defer_set`) and the last command wins at the merge, so two `hp -= 10` from `hp = 100` give 90, not 80. With
  `modify` it must be 80.
