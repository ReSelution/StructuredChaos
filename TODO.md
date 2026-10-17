# TODO

Found while integrating `sc::ecs2` into [ecs_benchmark](https://github.com/abeimler/ecs_benchmark)
(`src/structured-chaos`, pinned to commit `872ede0`).

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
- [ ] **No bulk destroy:** `destroyBulk` is a loop, as for gaia and flecs, so `BM_DestroyEntitiesInBulk` measures the
  same as `BM_DestroyEntities`.
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
