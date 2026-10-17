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

- [ ] **No `emplace_or_replace`:** the benchmark's `addComponents` checks with `contains` and otherwise assigns `T{}`,
  like EnTT.
- [x] **No bulk destroy:** `World::destroy(std::span<const Entity>)` since `fa865dc`; the factory of ecs_benchmark
  uses it since its commit `9bf8775`.
- [ ] **No entity count:** the benchmark tests check with `valid()` instead.

## Events with immediate delivery

Goal: a system emits an event and its listeners run right away, on the emitting thread, also from `addParallel`.

- [ ] **`World::modify<Component>(entity, function)`:** calls `function` with the component while holding its
  `making` bit (taken with the same spin as `Column::erase`), then releases it; false if the entity does not have
  the component. Today the bit only guards the existence of a component (`tryEmplace`, `erase`), not its value:
  `get()` returns `*valueAt(index)` and `eachInPages` reads and writes without it. So two listeners that hit the same
  target from two threads both write `Health` at once, and an `erase` does not wait for a thread that still uses the
  value (see the comment on `World::parallelEach`). With `modify` the second listener and the `erase` wait, the
  component does not have to be atomic, and the SIMD loop of full pages stays as it is; only the `modify` path pays
  for the atomic.
- [ ] **Listeners declare their access:** `scheduler.on<Hit>([](const Hit &, Query<const Armor>, Commands) { ... })`
  with the same `SystemParam`s as systems, and an `Emit<Hit>` parameter for the emitter whose `declare` adds the
  `SystemAccess` of every listener of `Hit`. The stages are built in `run()`, so listeners only have to be registered
  before that. Needed even with `modify`: a plain system that writes the same component through `each`/`get` in the
  same stage does not take the bit, so either the scheduler knows the listener writes it, or every writer of that
  component goes through `modify`.
- [ ] **Measure:** cost of `modify` per event, and with many threads waiting on the same target (one boss, many
  hits), as a case in `tests/ecs_compare`.

## `modify` inside parallel loops: directions, not decided

The API is not settled; these are the open questions found so far.

- [ ] **Same-loop race:** `modify` only helps if every writer takes the lock. A `parallelEach<Health>` writes its own
  pages without it; if its callback calls `modify<Health>` on an entity in a page another thread is iterating, both
  write the same `Health`. Either the scheduler/an assert forbids `modify<T>` inside a loop that writes `T`, or the
  loop locks what it iterates (next point).
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
- [ ] **Nested `modify`:** a callback that calls `modify` on another entity can deadlock (A holds e1 wants e2, B holds
  e2 wants e1). Simplest: forbid it with a thread-local flag and an assert.
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
- [ ] **Bulk destroy over threads:** pages are independent, so the second step could be split over threads by
  page, without atomics if every page belongs to one thread. Only worth it for many pages (waking threads costs
  microseconds); not tried.
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
- [ ] **Bulk create at 262K still behind flecs (6.3 against 3.5):** not measured. Guess: once all entities are gone,
  every page but the few spares gives its memory back (`park`/`vacate`), and the next create faults it in again
  (`clear_highpages`, `do_anonymous_page`, `asm_exc_page_fault` in the profile, 2–3 %); flecs keeps its memory. If
  so, a trade-off of ecs2 and not a fault; check with perf before changing anything.
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
