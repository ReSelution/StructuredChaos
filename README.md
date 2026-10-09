# StructuredChaos

<p align="center">
  <img src="img/sc_logo.png" alt="StructuredChaos Logo" width="300"/>
</p>

<p align="center">
  <a href="https://github.com/ReSelution/StructuredChaos/actions/workflows/ci.yml"><img src="https://github.com/ReSelution/StructuredChaos/actions/workflows/ci.yml/badge.svg" alt="CI"/></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License: MIT"/></a>
  <img src="https://img.shields.io/badge/C%2B%2B-23-blue.svg" alt="C++23"/>
</p>

A compact C++23 library of building blocks for games and other real-time
applications: an ECS with a system scheduler, a priority thread pool, heap-based memory
management, logging, statistics, localization and hashing.

## Modules

| Module | Header | What it does |
| --- | --- | --- |
| [ECS](#ecs) | `sc/ecs/scheduler.hpp` | EnTT registry without locks, and a scheduler that runs the systems that do not conflict at the same time |
| [Threading](#threading) | `sc/threading/threading.hpp` | Thread pool with priorities, futures, batches and parallel loops |
| [Memory](#memory) | `sc/memory/heap.hpp` | mimalloc heap that frees everything at once, usable as a `std::pmr` resource |
| [Logger](#logger) | `sc/logger/logger.hpp` | Compile-time named loggers on top of spdlog, one file per module |
| [Stats](#stats) | `sc/stats/stats.hpp` | Counters and throughput meters that compile to nothing when disabled |
| [Localization](#localization) | `sc/localizations/magpie_string.hpp` | *Magpie*: translated strings looked up by a key hashed at compile time |
| [Hash](#hash) | `sc/hash/hash.hpp` | rapidhash with identical results at compile time and at run time |

### ECS

`sc::ecs::Registry` wraps an `entt::registry` and reaches components without
any locking. What keeps threads apart is the `sc::ecs::Scheduler`: a system is
a function whose parameters say what it reads and writes, and two systems run
at the same time if neither writes a component the other one uses.

```cpp
#include "sc/ecs/registry.hpp"
#include "sc/ecs/scheduler.hpp"
#include "sc/threading/threading.hpp"

struct Position { float x{0.0f}; float y{0.0f}; };
struct Velocity { float dx{0.0f}; float dy{0.0f}; };
struct Health { float value{100.0f}; };
struct Frozen {};

using sc::ecs::Commands;
using sc::ecs::Exclude;
using sc::ecs::View;

sc::threading::init(); // the scheduler runs its systems on the thread pool

sc::ecs::Registry registry;

sc::ecs::Entity e = registry.create();
registry.emplace<Position>(e, 1.0f, 2.0f);
registry.emplace<Velocity>(e, 0.5f, -0.5f);
registry.emplace<Health>(e);

sc::ecs::Scheduler scheduler;

// Writes Position, reads Velocity: a const component is only read.
scheduler.add([](View<Position, const Velocity, Exclude<Frozen>> view) {
  for (auto [entity, pos, vel] : view.each()) {
    pos.x += vel.dx;
    pos.y += vel.dy;
  }
});

// The body of a loop, called for every entity on several threads at once.
// Touches other components than the system above and runs next to it.
scheduler.addParallel([](Health &health) { health.value = std::min(100.0f, health.value + 1.0f); });

// Adding and removing components and destroying entities is recorded and
// done once the systems running at the same time are finished.
scheduler.add([](View<const Health> view, Commands<Frozen> commands) {
  for (auto [entity, health] : view.each()) {
    if (health.value <= 0.0f) {
      commands.destroy(entity);
    } else if (health.value < 10.0f) {
      commands.emplace<Frozen>(entity);
    }
  }
});

scheduler.run(registry); // once per frame
```

A system may take any number of views, groups and one `Commands`. One that
takes `sc::ecs::Registry &` may do anything and runs alone. Systems that cannot
run together keep the order in which they were added.

`addParallel` builds the view from the parameters of the function. A view with
excluded components or a group is named instead:

```cpp
#include "sc/ecs/group.hpp"

scheduler.addParallel<View<Position, const Velocity, Exclude<Frozen>>>(
    [](Position &pos, const Velocity &vel) { pos.x += vel.dx; });

// A group keeps its components packed together and is the fastest to go through.
scheduler.addParallel<sc::ecs::Group<entt::owned_t<Position, const Velocity>>>(
    [](Position &pos, const Velocity &vel) { pos.x += vel.dx; });
```

Before the components, the function may take the entity and `Commands`. Every
thread records into a buffer of its own; the changes of different threads are
made in no particular order:

```cpp
scheduler.addParallel([](entt::entity entity, Commands<Frozen> commands, const Health &health) {
  if (health.value <= 0.0f) {
    commands.destroy(entity);
  } else if (health.value < 10.0f) {
    commands.emplace<Frozen>(entity);
  }
});
```

Inside a system that does more than one loop, `sc::ecs::parallelEach(view,
function)` does the same for a single view or group and `sc::ecs::each(view,
function)` on the calling thread alone. Both go through the components page by
page, which is several times faster than the `each` of an EnTT view for a
function that does little. Such a system records changes from its loops through
`sc::ecs::ParallelCommands`, whose `local()` is the `Commands` of the calling
thread.

Without the scheduler, the registry is used like an `entt::registry`:

```cpp
auto [pos, vel] = registry.get<Position, const Velocity>(e);
pos.x += vel.dx;

for (auto [entity, position] : registry.view<Position, Exclude<Frozen>>().each()) {
  position.y = 0.0f;
}

registry.destroy(e);
```

Used from several threads this way, a component type must either be read by
any number of threads or written by a single one; adding and removing
components counts as writing. Only `create` and `valid` are safe from any
thread at any time.

Components have to be trivially destructible. Variable-sized data goes into a
`sc::ecs::Resource<std::span<T>>` field, which the registry allocates from a
heap of that component and frees together with it.

### Threading

```cpp
#include "sc/threading/threading.hpp"

sc::threading::init(); // hardware_concurrency() - 1 workers

// A task with a result
std::future<int> answer = sc::threading::enqueue([] { return 42; });

// One task per element, results collected in order
std::vector<int> numbers{1, 2, 3, 4};
auto squares = sc::threading::enqueueBatch(numbers, [](int n) { return n * n; });
std::vector<int> result = squares.get(); // {1, 4, 9, 16}

// Fire and forget, with a priority
using sc::threading::Priority;
sc::threading::detach<Priority::Low>([] { /* ... */ });

sc::threading::waitUntilFinished();
```

`detachBatch` does the same for a range and calls an optional callback once
the last task has finished.

`parallelFor` runs a loop on the calling thread and on every worker that has
nothing else to do, and returns once it is done:

```cpp
std::vector<float> samples(1'000'000);

// Chunks of at most 4096 items, called from several threads at once.
sc::threading::parallelFor(samples.size(), 4096, [&](size_t first, size_t last) {
  for (size_t i = first; i < last; ++i) {
    samples[i] *= 0.5f;
  }
});
```

The calling thread never sleeps while it waits, so `parallelFor` may also be
called from a task of the pool or from inside another `parallelFor`.

### Memory

`sc::Heap` owns a mimalloc heap. Nothing has to be freed one by one:
`reset()` and the destructor release every block at once and run the
destructors of the objects created with `make()`.

```cpp
#include "sc/memory/heap.hpp"

sc::Heap heap;

// An object that lives as long as the heap does
auto *names = heap.makePmr<std::pmr::vector<std::pmr::string>>();
names->emplace_back("lives entirely inside the heap");

// Uninitialized room for 1024 floats
std::span<float> samples = heap.allocateSpan<float>(1024);

heap.reset(); // everything above is gone
```

Allocating is safe from several threads at the same time.

### Logger

```cpp
#include "sc/logger/logger.hpp"

using NetLog = sc::Logger<"Net">;
using NetIoLog = sc::Logger<"Net", "IO">; // module "Net", category "IO"

NetLog::init(spdlog::level::debug); // console level, optional
NetLog::info("listening on port {}", 8080);
NetIoLog::warn("retrying {}", "handshake");

{
  // Logs "<duration> to load config.json" when it goes out of scope.
  auto timer = NetLog::time("{} to load {}", "config.json");
}
```

Every module writes to `logs/<module>.log`, shared by all of its categories.
The file receives every level, the console only those from the configured one
upwards. `sc::setLogDirectory()` changes the directory.

### Stats

```cpp
#include "sc/stats/counter.hpp"
#include "sc/stats/stats.hpp"

using DrawCalls = sc::stats::Stat<"Draw Calls", sc::stats::Counter<>>;

DrawCalls::record(3);

sc::stats::reportAll<NetLog>(); // logs every stat of the program
```

Stats are off by default and then cost nothing: every call is an empty
function. Configure with `-Denable_chaos_stats=true` to turn them on.

### Localization

```cpp
#include "sc/localizations/magpie_string.hpp"

using namespace sc::literals; // for _t

sc::Magpie::get().insert(sc::MagpieKey{"menu", "start"}, "Starten", "menu", "start");

std::string_view text = "menu:start"_t; // "Starten"
```

The `_t` literal hashes namespace and key at compile time, so a lookup at run
time is one probe into a hash map. For loading large tables from several
threads, `insertStoredLocal()` fills a thread-local map that `mergeLocal()`
then moves into the shared one.

### Hash

```cpp
#include "sc/hash/hash.hpp"

constexpr sc::h64 id = sc::hash("player");

using namespace sc::literals;
static_assert("player"_h == id);

bool same(std::string_view name) { return sc::hash(name) == id; }
```

## Requirements

* A C++23 compiler. CI builds with Clang 22 on Linux and clang-cl on Windows.
* [Meson](https://mesonbuild.com) 1.1 or newer and Ninja.

All dependencies are fetched as Meson subprojects if they are not installed.

## Build

```bash
meson setup build
meson compile -C build
meson test -C build --suite StructuredChaos
```

`cross/linux-clang.ini` selects Clang with mold and ccache:

```bash
meson setup build --native-file cross/linux-clang.ini
```

Benchmarks run separately. The ones of the ECS compare it with a bare
`entt::registry` and want an optimized build (`-Dbuildtype=release
-Db_ndebug=true`):

```bash
meson test -C build --benchmark
```

### Options

| Option | Default | Effect |
| --- | --- | --- |
| `enable_chaos_stats` | `false` | Compiles the stats in instead of leaving them out |
| `dump_magpie` | `false` | Keeps namespace and key of every text, for `Magpie::dump()` |
| `mimalloc_new_delete` | `true` | On Windows, routes `new`/`delete` of every executable that uses `sc_dep` to mimalloc |

```bash
meson setup build -Denable_chaos_stats=true
```

### Sanitizers

Sanitizer builds are selected with Meson's built-in `b_sanitize` option and need Clang.
mimalloc is reconfigured to match automatically.

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer
meson setup build-asan --native-file cross/linux-clang.ini --buildtype=debug -Db_sanitize=address,undefined
meson test -C build-asan

# ThreadSanitizer
meson setup build-tsan --native-file cross/linux-clang.ini --buildtype=debug -Db_sanitize=thread
meson test -C build-tsan
```

## Using it in a Meson project

Add `subprojects/StructuredChaos.wrap`:

```ini
[wrap-git]
url = https://github.com/ReSelution/StructuredChaos.git
revision = master

[provide]
structured_chaos = sc_dep
```

and depend on it:

```meson
sc_dep = dependency('structured_chaos')

executable('game', 'main.cpp', dependencies: sc_dep, override_options: ['cpp_std=c++23'])
```

## Dependencies

| Library | Used for |
| --- | --- |
| [EnTT](https://github.com/skypjack/entt) | ECS storage |
| [mimalloc](https://github.com/microsoft/mimalloc) | Heaps and the global allocator |
| [spdlog](https://github.com/gabime/spdlog) | Logging |
| [rapidhash](https://github.com/Nicoshev/rapidhash) | Hashing |
| [unordered_dense](https://github.com/martinus/unordered_dense) | Hash maps |
| [simdutf](https://github.com/simdutf/simdutf) | UTF-16 to UTF-8 conversion |
| [atomic_queue](https://github.com/max0x7ba/atomic_queue) | Lock-free queues of the thread pool |
| [PFR](https://github.com/boostorg/pfr) | Reflection over component fields |
| [glm](https://github.com/g-truc/glm), [xsimd](https://github.com/xtensor-stack/xsimd) | Math and SIMD, passed on to consumers |
| [Catch2](https://github.com/catchorg/Catch2) | Tests and benchmarks |

## License

[MIT](LICENSE)
