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
applications: a thread-safe ECS, a priority thread pool, heap-based memory
management, logging, statistics, localization and hashing.

## Modules

| Module | Header | What it does |
| --- | --- | --- |
| [ECS](#ecs) | `sc/ecs/registry.hpp` | EnTT registry with a lock per component, safe to use from many threads |
| [Threading](#threading) | `sc/threading/threading.hpp` | Thread pool with priorities, futures and batches |
| [Memory](#memory) | `sc/memory/heap.hpp` | mimalloc heap that frees everything at once, usable as a `std::pmr` resource |
| [Logger](#logger) | `sc/logger/logger.hpp` | Compile-time named loggers on top of spdlog, one file per module |
| [Stats](#stats) | `sc/stats/stats.hpp` | Counters and throughput meters that compile to nothing when disabled |
| [Localization](#localization) | `sc/localizations/magpie_string.hpp` | *Magpie*: translated strings looked up by a key hashed at compile time |
| [Hash](#hash) | `sc/hash/hash.hpp` | rapidhash with identical results at compile time and at run time |

### ECS

`sc::ecs::Registry` wraps an `entt::registry` and gives every component type
its own reader/writer lock. Writes take the lock of one component; `get`,
`view` and `group` return their result together with a shared lock that is
held for as long as the result lives.

```cpp
#include "sc/ecs/registry.hpp"

struct Position { float x{0.0f}; float y{0.0f}; };
struct Velocity { float dx{0.0f}; float dy{0.0f}; };

sc::ecs::Registry registry;

sc::ecs::Entity e = registry.create();
registry.emplace<Position>(e, 1.0f, 2.0f);
registry.emplace<Velocity>(e, 0.5f, -0.5f);

{
  // The lock is released at the end of the scope.
  auto [lock, pos, vel] = registry.get<Position, Velocity>(e);
  pos.x += vel.dx;
}

{
  auto view = registry.view<Position, Velocity>();
  view.raw().each([](entt::entity, Position &pos, const Velocity &vel) {
    pos.x += vel.dx;
    pos.y += vel.dy;
  });
}

registry.destroy(e);
```

Components have to be trivially destructible. Variable-sized data goes into a
`sc::ecs::Resource<std::span<T>>` field, which the registry allocates from a
heap of that component and frees together with it.

A thread that holds a lock must not ask the same registry for another one on
the same components; a writer waiting in between would block it for good.

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

sc::threading::wait_until_finished();
```

`detachBatch` does the same for a range and calls an optional callback once
the last task has finished.

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

sc::stats::report_all<NetLog>(); // logs every stat of the program
```

Stats are off by default and then cost nothing: every call is an empty
function. Configure with `-Denable_chaos_stats=true` to turn them on.

### Localization

```cpp
#include "sc/localizations/magpie_string.hpp"

sc::MagpieKey key{"menu", "start"};
sc::Magpie::get()->insert(key, "Starten", "menu", "start");

std::string_view text = "menu:start"_t; // "Starten"
```

The `_t` literal hashes namespace and key at compile time, so a lookup at run
time is one probe into a hash map. For loading large tables from several
threads, `mt_InsertStored()` fills a thread-local map that `mt_Merge()` then
moves into the shared one.

### Hash

```cpp
#include "sc/hash/hash.hpp"

constexpr sc::h64 id = sc::hash("player");

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

Benchmarks are part of the test binaries and run separately:

```bash
meson test -C build --benchmark
```

### Options

| Option | Default | Effect |
| --- | --- | --- |
| `enable_chaos_stats` | `false` | Compiles the stats in instead of leaving them out |
| `dump_magpie` | `false` | Keeps namespace and key of every text, for `Magpie::dump()` |

```bash
meson setup build -Denable_chaos_stats=true
```

### Sanitizers

Sanitizer builds are selected with Meson's built-in `b_sanitize` option and need Clang.
mimalloc is reconfigured to match automatically.

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer
meson setup build-asan --native-file cross/linux-clang.ini -Db_sanitize=address,undefined
meson test -C build-asan

# ThreadSanitizer
meson setup build-tsan --native-file cross/linux-clang.ini -Db_sanitize=thread
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
| [PFR](https://github.com/boostorg/pfr) | Reflection over component fields |
| [glm](https://github.com/g-truc/glm), [xsimd](https://github.com/xtensor-stack/xsimd) | Math and SIMD, passed on to consumers |
| [Catch2](https://github.com/catchorg/Catch2) | Tests and benchmarks |

## License

[MIT](LICENSE)
