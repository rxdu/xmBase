# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

xmBase is the foundation library of the XMotion family — the shared substrate every other component builds on. Because it is depended on by *all* components, it deliberately contains only what is universal across the whole family:
- **Telemetry API + logging**: the stateless XMotion instrumentation surface (`xmbase/telemetry/`) with a built-in dependency-free console binding — the `XM_*` macros are both the logging front-end and the telemetry `event()` verb
- **Common types**: the shared geometry/primitive type vocabulary (`xmbase/types/`, namespace `xmotion`) spoken by both the driver layer (xmDriver) and the motion layer (xmNavigation)
- **Concurrency primitives** (`xmbase/concurrency/`, namespace `xmotion::concurrency`): the family's verified seqlock MessageBuffer, SPSC SpscQueue, waiter and placement policies — promoted from xmMessaging per ADR 0007
- **Testing/bench toolkit** (`xmbase/testing/`, namespace `xmotion::testing`): the unified family measurement harness + allocation probe — test/bench tier ONLY, never linked by runtime code

It is exposed as **two** CMake targets since 0.5.0 (ADR 0007 target split): `xmotion::xmBase` — the Eigen-FREE core (default; telemetry, wire-tier types, serialization, concurrency, testing) — and `xmotion::xmBaseGeometry` — the Eigen geometry tier (`types/geometry.hpp`, `types/quantities.hpp`), linked *in addition to* the core by geometry consumers (xmDriver, xmNavigation). Anything particular to an upper layer — driver/control interfaces, motion-specific types — lives in its owning component (xmDriver, xmNavigation), not here.

The library is designed to be used either as a standalone project or as a module embedded in other projects.

## Build System

### CMake Configuration

**Standard build:**
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

**Build with tests:**
```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build .
ctest
```

**Development mode** (forces building tests):
```bash
cmake .. -DXMOTION_DEV_MODE=ON
```

**Run a single test** (tests are gtest, discovered by ctest):
```bash
cmake --build .
ctest -R ConsoleBinding --output-on-failure
```

### CMake Options

- `BUILD_TESTING`: Build tests (default: OFF)
- `XMOTION_DEV_MODE`: Development mode, forces building tests (default: OFF)
- `ENABLE_LOGGING`: Compile the built-in console binding (default: ON; OFF = no binding auto-adopts)
- `XMBASE_GEOMETRY`: Build the Eigen geometry tier target `xmBaseGeometry` (default: ON; OFF = core-only build, the Eigen-free proof configuration — no Eigen needed anywhere)
- `STATIC_CHECK`: Enable cppcheck static analysis (default: OFF)

### Dependencies

**Required:**
- CMake >= 3.10.2
- C++17 compiler

**Geometry tier only** (`XMBASE_GEOMETRY=ON`, the default):
- Eigen3

**Install on Ubuntu:**
```bash
sudo apt-get install libeigen3-dev
```

There are no other third-party runtime dependencies — the console binding is libc/libstdc++ only, and the core target needs no Eigen.

## Architecture

### Module Structure

Headers live under `include/xmbase/`; the compiled telemetry bindings under `src/`. Everything except the geometry tier compiles into the core target `xmotion::xmBase`; the geometry tier is the header-only `xmotion::xmBaseGeometry` (which links the core plus `Eigen3::Eigen`).

1. **telemetry/** (`include/xmbase/telemetry/`): the stateless XMotion instrumentation surface (ADR 0004) — 4 verbs (`event`/`metric`/`scope`/`signal`) + health, context spine (TraceId/Context/NewTrace/Inject/Extract), install-once binding seam (`binding.hpp`). Compiled parts: `src/telemetry_unbound.cpp` (fallback + adoption latch) and `src/telemetry_console_binding.cpp` (the built-in dependency-free console binding, auto-adopted by default).
2. **types/** (`include/xmbase/types/`): Header-only common type vocabulary (namespace `xmotion`), split across the two targets
   - Wire tier (CORE target, Eigen-free): `scalar.hpp` (enum base), `time.hpp` (`Clock`/`Timestamp`/`Duration`), `vector.hpp` (POD `vector3_t`/`vector4_t` for the wire/driver layer), `stamped.hpp` (`Stamped<T>`), and the `base_types.hpp` facade (bundles exactly those).
   - Geometry tier (`xmBaseGeometry`): `geometry.hpp` (Eigen-backed pose/velocity/joint/wrench + `Pose`/`Twist`/`Odometry`), `quantities.hpp` (**opt-in** strong-typed quantities — tagged `Vec3`: `Force`, `Torque`, `LinearVelocity`, …; reach Eigen via `.vec()`; not included by the umbrella), the `geometry_types.hpp` facade, and `types.hpp` — the umbrella pulls geometry and is therefore a GEOMETRY-TIER header (documented in the header; core consumers include granular headers instead).
   - Conventions: SI units, radians, intrinsic Z-Y-X Euler, Hamilton quaternions; all composite types default-initialize to identity/zero.
   - These are the types shared by *both* the driver and motion layers; layer-specific types (e.g. trajectories) live in those layers.
3. **concurrency/** (`include/xmbase/concurrency/`, namespace `xmotion::concurrency`): the family's verified concurrency primitives, promoted from xmMessaging `detail/` at ADR 0007 W1 — `MessageBuffer` (wait-free seqlock depth-1 exchange, Boehm construction), `MutexMessageBuffer` (rich-type fallback), `SpscQueue` (lock-free SPSC ring), `EventCount`/`CondvarEventCount` (eventcount parking), `HeapStorage`/`RegionStorage` (the storage seam that lets one algorithm serve heap and shared-region cells). These are a MOVE of verified code: do not "improve" the algorithms; the memory-ordering proofs in the headers are load-bearing and any change re-runs the full TSan/aarch64 verification.
4. **testing/** (`include/xmbase/testing/`, namespace `xmotion::testing`): the unified family bench harness (`bench_harness.hpp` — tail percentiles, batched measurement loops, hardware context, JSON reports) and allocation probe (`alloc_probe.hpp` — include from exactly ONE translation unit per binary; it replaces global operator new/delete). Test/bench tier ONLY — never link it from runtime code. JSON field names `name`/`p50`/`p99`/`max` are load-bearing (xmMessaging's `compare.py` reads them).
5. **container/ + event/**: DEPRECATED-FOR-REMOVAL at 0.6.0 (ADR 0007) — still present and compiled, but new code must not add includes.

### Key Design Patterns

**Two-target foundation (0.5.0, ADR 0007):**
- `xmBase` is the STATIC core library (telemetry API + wire-tier types + serialization + concurrency + testing) with NO Eigen anywhere in its interface; `xmBaseGeometry` is a header-only INTERFACE target carrying the Eigen tier. Consumers `find_package(xmBase)` and link `xmotion::xmBase` (plus `xmotion::xmBaseGeometry` if they use geometry types; `COMPONENTS geometry` gives a clear configure-time diagnostic when Eigen is missing).
- The core's Eigen-freedom is CI-enforced (the `eigen-free-core` job builds with no libeigen3-dev installed). Never add an Eigen include to a core-tier header.
- There are intentionally no driver/control interfaces here — they belong to xmDriver's HAL. Keeping xmBase free of upper-layer specifics is a load-bearing design rule, not an accident.

**Telemetry API (xmbase/telemetry/, ADR 0004):** components instrument against this API only; the runtime machinery lives in the optional (privately maintained) xmTelemetry SDK, bound through the install-once seam. Binding states: console binding auto-adopted by default (full-severity console logging, dependency-free); explicit `InstallBinding` — including `nullptr` — is authoritative and disables auto-adoption; with no binding, events ≥ Warn and non-Ok health go to stderr, everything else no-ops. Do NOT put machinery under `xmbase/telemetry/` — that directory is the stateless API tier by definition. **Never document SDK internals in this repo** — public docs describe the API and the SDK's guarantees, not its mechanisms.

**Logging (ONE API with telemetry):**
- The `XM_*` macros ARE the logging front-end (the former `XLOG_*` spelling was removed — clean break), rendered by the console binding by default and captured by the SDK when bound.
- Macro-based logging API that compiles out when `ENABLE_LOGGING` is disabled; the compile-time `XM_TELEMETRY_LEVEL` floor strips below-floor call sites entirely.
- The console binding is synchronous and thread-safe (whole lines, never torn), colorized on a tty.

### Namespace Convention

All code uses the `xmotion` namespace.

## Logging Configuration

- `XM_LOG_LEVEL`: runtime log level, seeded once at startup (default: 2) — 0: Trace, 1: Debug, 2: Info, 3: Warn, 4: Error, 5: Fatal, 6: Off; `SetLogLevel()` at runtime.
- File logging and structured export are SDK capabilities, not part of the console binding.

**Usage in code** (format strings use `{}` placeholders with the documented spec subset, **not** printf):
```cpp
#include "xmbase/telemetry/telemetry.hpp"

XM_INFO("Motor speed: {} RPM", speed);
XM_DEBUG_STREAM("Position: " << x << ", " << y);   // non-RT convenience
```

**Important:** Do not make logging calls after a signal is received (undefined behavior).

## CI/CD

### GitHub Actions Workflows

**C++ Workflow** (`.github/workflows/cpp.yml`): ubuntu-22.04 + ubuntu-24.04; separate build and test jobs (`-DBUILD_TESTING=ON`); `ubuntu-24.04-arm` build+test (weak memory model); `eigen-free-core` (no Eigen installed, `-DXMBASE_GEOMETRY=OFF` — the core Eigen-freedom proof); sanitizer lanes (ASan/UBSan full suite; TSan on the console-binding + concurrency suites, with the documented `-Wno-tsan` for GCC 13's fence warning).

**ROS Workflow** (`.github/workflows/ros.yml`): builds with ROS Humble and Jazzy via colcon.

## Development Guidelines

### Code Style

- **C++ Standard**: C++17 (set in CMakeLists.txt)
- **File Extensions**: `.cpp` for source, `.hpp` for headers
- Follow existing naming conventions in the codebase
- Use clang-format with Google style for formatting

### What belongs here (and what doesn't)

Add to xmBase only things every component could share: the instrumentation surface, or a type that is genuinely spoken by more than one layer. Concretely:
- A new **common type** goes in `include/xmbase/types/` (header-only; no CMakeLists change needed).
- A new **driver/control interface** does **not** go here — it belongs to its owning component. If a would-be "common" type is only used by one upper layer, put it in that layer instead.

### Testing

- Tests use GoogleTest (bundled in `third_party/googletest`)
- Test files live in the top-level `test/` directory (e.g. `test/test_logging.cpp` — the console-binding suite; `test/test_telemetry_api.cpp` — the seam/ABI contract; `test/test_concurrency_*.cpp` — the primitive verification suite)
- Add new tests to `test/CMakeLists.txt`; core-tier tests link `xmBase` only, geometry-tier tests link `xmBaseGeometry` (guarded by `if (TARGET xmBaseGeometry)`)
- The console-binding and concurrency suites run under TSan in CI (gtest suite names carrying "Concurrency" are what the TSan ctest filter selects — keep the naming); the full suite runs under ASan/UBSan
- Primitive benchmarks live in `bench/` (`xmbase_bench`, built with tests; `--smoke` run registered with ctest; JSON report via `--out`)
- All test executables output to `build/bin/`

### Build Modes

The project supports two build contexts:
- **Standalone**: `BUILD_AS_MODULE=OFF` (when built as top-level project)
- **Module**: `BUILD_AS_MODULE=ON` (when included via add_subdirectory)

Tests are only built in standalone mode with `BUILD_TESTING=ON` or when `XMOTION_DEV_MODE=ON`.

## Installation and Packaging

**Install to system:**
```bash
cmake --install . --prefix /usr/local
```

**Create Debian package:**
```bash
cpack
```

Package details:
- Package name: libxmotion-base
- Default install prefix: /opt/xmotion
- Exports two CMake targets, `xmotion::xmBase` (Eigen-free core) and `xmotion::xmBaseGeometry` (Eigen tier), via `find_package(xmBase)` — one repo, one release, one deb
- Includes CMake config files for find_package() support

## ROS Integration

The library can be built with colcon in a ROS workspace: place in `src/` and run `colcon build`. Time mapping to ROS clocks happens at the application boundary, not in this library.
