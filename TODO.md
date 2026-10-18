# TODO

Found while integrating `sc::ecs2` into [ecs_benchmark](https://github.com/abeimler/ecs_benchmark)
(`src/structured-chaos`, pinned to commit `bfd4be4`).

## Decisions of the benchmark integration to review

- **Single thread:** the `World` wrapper of the benchmark calls `sc::threading::init(0)` once, the first time a world
  is made. Without it the benchmark thread has no slot of its own in `EntityAllocator::withShare`, and every
  `create()` goes through `m_sharedMutex`. With `0` no workers run, so it stays single-threaded like the other
  frameworks.
- **No malloc override:** mimalloc is built with `MI_OVERRIDE OFF`. ecs2 only uses mimalloc through its own `mi_*`
  calls; the rest of the benchmark runs on the system malloc, like every other framework there. `tests/ecs_compare`
  instead uses mimalloc for all libraries.
- **fmt from vcpkg:** spdlog is built with `SPDLOG_FMT_EXTERNAL`. `SPDLOG_USE_STD_FORMAT` fails with both gcc and
  clang because `sc/logger/logger.hpp:139` uses `fmt_lib::runtime`, which `std` does not have. The bundled fmt does
  not work either: it has the same include guards as the `<fmt/core.h>` that the benchmark's
  `base/systems/DataSystem.h` includes, and both end up in the same translation units.

## Missing API in ecs2

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

## Limits

- [ ] **`INDEX_LIMIT` beyond 2^24:** the limit comes from the version before the chunks, which reserved the address
  space of every column for all indices (2^32 × 256 bytes = 1 TiB per column). With values in chunks, what is left
  that grows with the limit (looked at in the code, nothing measured):
  - The entity handle already has 32 bits of index (`INDEX_BITS = 32`), and the values only take address space for
    the chunks in use: no work.
  - The page headers lie in reserved address space and are found from the index: at 2^32 about 1.1 GB of address
    space per column (`PAGE_COUNT × 1088 B`), untouched where unused; with 128 TiB that is room for ~100 000 columns.
    No problem.
  - **The one hard limit:** `m_headerBlocks` is a single `uint64_t`, one bit per committed header block, so that
    `findHeader` never reads a reservation without memory (needed on Windows); `static_assert(HEADER_BLOCK_COUNT <=
    64)`. At 2^32 there are 16 384 blocks: an array of masks (256 words), or a second level as with the summary.
  - Real memory instead of address space: `m_chunks`, `m_chunkMemory`, `m_chunkKept` are `std::array`s in the
    `Column` object, sized `INDEX_LIMIT / CHUNK_SIZE` and zeroed when it is made; at 2^32 and 256-byte components
    ~9 MB per column. In reserved address space like the headers, a table would only cost what is touched.
  - The same for `m_killPlace` of the bulk destroy (`fa865dc`): a `std::vector` of `PAGE_COUNT` entries, 16 KB per
    world now, 4 MB at 2^32. Reserved instead, only the entries of the touched pages get memory, and destroy sets
    them back to 0 anyway.
  - A middle way: 2^28 (268 M entities) needs 1024 header blocks, 16 words instead of one, and nothing else changes
    much.

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
- [ ] **Listeners declare their access:** `scheduler.on<Hit>([](const Hit &, Query<const Armor>, Commands) { ... })`
  with the same `SystemParam`s as systems, and an `Emit<Hit>` parameter for the emitter whose `declare` adds the
  `SystemAccess` of every listener of `Hit`. The stages are built in `run()`, so listeners only have to be registered
  before that. Needed even with `modify`: a plain system that writes the same component through `each`/`get` in the
  same stage does not take the bit, so either the scheduler knows the listener writes it, or every writer of that
  component goes through `modify`.
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

## `modify` inside parallel loops: directions, not decided

The API is not settled; these are the open questions found so far.

- [x] **Same-loop race:** `modify` only helps if every writer takes the lock. A `parallelEach<Health>` writes its own
  pages without it; if its callback calls `modify<Health>` on an entity in a page another thread is iterating, both
  write the same `Health`. For systems a `static_assert` in `Scheduler::addSystem` now forbids `Modify<T>` together
  with `T` as a loop parameter or in a query (also `const T`: reading without the bit next to a `modify` is a race
  as well). Not checked for `world.parallelEach` with `world.modify` in it, which has no types to check.
  The page lock below would allow it.
- [ ] **Page lock while iterating:** a loop locks the page of each column it writes (per column and page, not per
  entity page, so a loop over `Health` does not block `modify<Sprite>`), one RMW per 4096 entities. Pitfalls:
  - Deadlock: thread A holds page P1 and calls `modify` on P2, thread B holds P2 and calls `modify` on P1 (two groups
    hitting each other). `modify` must never block while its thread holds a page lock.
  - Latency: a waiting `modify` waits up to a whole page; many threads hitting one target (the player's page) all
    wait for one thread.
- [ ] **Non-blocking `modify` (candidate):** the page lock holds the owning thread id.
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
  the `if`s of the benchmark's Health/Damage logic, not in the loop of ecs2, and they are not the bottleneck: with
  clang 3k fewer, same time. Tried without gain: a linear loop with a bit test for words at least half full (+6 to
  +17 %), pointers moved on by the gap instead of `values + slot` (+6 % instructions, same cycles). What is left is
  the layout; see "Entity locality" below.

Entity benchmarks, ns per entity, rank among 7–8 frameworks (mustache has no destroy/remove-add results). Create and
destroy from the reworked benchmarks of ecs_benchmark (`c00a2d1`: one registry for all iterations, so indices are
reused; 1, 64, 1024 and 262K entities, 0.3 s each), the rest from the full run. Before the rework every create and
destroy result held the teardown of a registry per iteration (sc::ecs2 15 µs, flecs 470 µs), which made sc look
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
- [ ] **+3 to 5 % on unchanged paths after the parallel `giveMany`:** in `ecs_compare_sc2`, destroy on one thread
  820 → 848 µs, all at once 781 → 821, add + remove recorded 541 → 564 (7 runs each), though their code did not
  change. The probe that calls `destroy(span)` directly shows no difference (100K 691 / 690 / 700 against 687 /
  702 / 689). Presumably code placement in that binary (new instantiations, other inlining); not looked into.
- [ ] **`World::destroy` must not run next to other threads, assert it:** the comment says so since `86d3592`
  (like `CommandList::apply`). Before, nothing said so and `eraseAll` even said "Safe next to threads that keep to
  other entities", while `FreeList::give` (`resize`, `push_back`) runs against `take` from `create` without a lock:
  thread A in `create` reads `indices[before - 1]` while thread B in `destroy` pushes back, the vector reallocates,
  A reads freed memory. Now also the components go without locked instructions. Left: a debug assert if ecs2 can
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
    entities per frame works elsewhere. No fault in ecs2 seen; only the bytes per entity could shrink, and the
    generation is the part that buys safety.
- `BM_AddComponent` says nothing: it pauses the timer per entity, every framework lands at ~540 ns.

## Entity locality: say at create which components an entity will get

Idea, not decided. Entities with the same set of components get their indices from the same pages, so their masks
are full and their components lie without gaps: the full-page/full-word loops apply and every loaded cache line is
used. Archetype locality at create, without the move on every later change. `create(rare)` is the small version of
it already (own pages at the top of the index space).

- [ ] **API:** something like `world.create(Layout<Position, Velocity, Health>{})`, or a handle made once
  (`auto heroes = world.layout<...>(); world.create(heroes);`) so that the lookup is not done per create.
- [ ] **Only a hint at create:** components added later do not move the entity, the index stays. In ecs_benchmark the
  mixed entities get their hero components only after create; there it helps only if the factory gives the layout
  at create. Legitimate use of the API, but the benchmark notes should say that sc gets a hint the others do not
  need.
- [ ] **Open pages per layout:** every layout keeps its own partly filled page. Many layouts with few entities give
  many half empty pages: more memory, and `each()` touches more pages (the summary softens it, the page loop and
  `page()` calls stay). A limit, or a fallback group for small layouts.
- [ ] **Reused indices:** today one free list for common and one for rare entities. For a destroyed hero to make room
  for a hero again, every layout needs its own free list; otherwise the index space mixes again over time (the state
  the reworked create benchmark of ecs_benchmark measures, with reused indices).
- [ ] **Threads:** `EntityAllocator::withShare` gives every thread ranges of its own; with layouts every thread needs
  a range per layout, or threads compete for the open page of a layout.
- [ ] **Measure the upper bound first:** a variant of the mixed update in which the hero entities get one contiguous
  index range (e.g. with `create(rare)` or bulk create). It shows how close sc gets to mustache/flecs with perfect
  locality; only if that pays off does the API.

## Show the parallel advantage in numbers

ecs_benchmark is single-threaded on purpose, so none of its results shows the main argument for ecs2 (structural
changes inside parallel loops without deferring).

- [ ] **Structural changes in a parallel loop:** per frame 1 %, 10 %, 50 % of the entities gain or lose a component
  inside a system that runs over all entities in parallel. sc: directly in `parallelEach`; flecs/gaia: deferred mode
  or command buffer plus flush; EnTT: collect and apply afterwards (no parallel `each`). Time per frame including the
  flush, at 1, 2, 4, 8 threads.
- [ ] **Lost updates:** many threads deal damage to few targets, the total must add up. flecs 4.0.1 in multi-threaded
  systems: `ensure`/`get_mut` hand out the existing component without a lock (data race), `set` is deferred
  (`flecs_defer_set`) and the last command wins at the merge, so two `hp -= 10` from `hp = 100` give 90, not 80. With
  `modify` it must be 80.
