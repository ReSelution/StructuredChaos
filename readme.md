# StructuredChaos

<p align="center">
  <img src="img/sc_logo.png" alt="StructuredChaos Logo" width="300"/>
</p>

A compact C++ library featuring modules for ECS, threading, memory management, stats, and more.

## Build & Setup

This project uses **Meson** as its build system.

```bash
# Configure the project and create the build directory
meson setup build

# Build the project
meson compile -C build

# Run tests
meson test -C build
```

## Dependencies

Key external dependencies

* **EnTT** (ECS Framework)
* **spdlog** (Fast C++ Logging)
* **Catch2** (Testing Framework)
* **mimalloc** (Performance Allocator)
* **glm**, **xsimd**, **simdutf**, **pfr**, **unordered_dense**
