# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

xmBase is the foundation library of the XMotion family — the shared substrate every other component builds on. Because it is depended on by *all* components, it deliberately contains only what is universal across the whole family:
- **Telemetry API + logging**: the stateless XMotion instrumentation surface (`xmbase/telemetry/`) with a built-in dependency-free console binding — the `XM_*` macros are both the logging front-end and the telemetry `event()` verb
- **Common types**: the shared geometry/primitive type vocabulary (`xmbase/types/`, namespace `xmotion`) spoken by both the driver layer (xmDriver) and the motion layer (xmNavigation)

It is exposed as a **single** CMake target, `xmotion::xmBase`. Anything particular to an upper layer — driver/control interfaces, motion-specific types — lives in its owning component (xmDriver, xmNavigation), not here.

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
- `STATIC_CHECK`: Enable cppcheck static analysis (default: OFF)

### Dependencies

**Required:**
- CMake >= 3.10.2
- C++17 compiler
- Eigen3

**Install on Ubuntu:**
```bash
sudo apt-get install libeigen3-dev
```

There are no other third-party runtime dependencies — the console binding is libc/libstdc++ only.

## Architecture

### Module Structure

Everything compiles into one target, `xmotion::xmBase`. Headers live under `include/xmbase/`; the compiled telemetry bindings under `src/`.

1. **telemetry/** (`include/xmbase/telemetry/`): the stateless XMotion instrumentation surface (ADR 0004) — 4 verbs (`event`/`metric`/`scope`/`signal`) + health, context spine (TraceId/Context/NewTrace/Inject/Extract), install-once binding seam (`binding.hpp`). Compiled parts: `src/telemetry_unbound.cpp` (fallback + adoption latch) and `src/telemetry_console_binding.cpp` (the built-in dependency-free console binding, auto-adopted by default).
2. **types/** (`include/xmbase/types/`): Header-only common type vocabulary (namespace `xmotion`)
   - Granular headers: `scalar.hpp` (enum base), `time.hpp` (`Clock`/`Timestamp`/`Duration`), `vector.hpp` (POD `vector3_t`/`vector4_t` for the wire/driver layer), `geometry.hpp` (Eigen-backed pose/velocity/joint/wrench + `Pose`/`Twist`/`Odometry`), `stamped.hpp` (`Stamped<T>`).
   - `types.hpp`: umbrella that pulls in all of the above.
   - `quantities.hpp`: **opt-in** strong-typed quantities (tagged `Vec3`: `Force`, `Torque`, `LinearVelocity`, …) — distinct types so the compiler catches quantity mix-ups; reach Eigen via `.vec()`. Not included by the umbrella.
   - `base_types.hpp` / `geometry_types.hpp`: compatibility facades re-exporting the granular headers (legacy include paths used by xmDriver/xmNavigation).
   - Conventions: SI units, radians, intrinsic Z-Y-X Euler, Hamilton quaternions; all composite types default-initialize to identity/zero.
   - These are the types shared by *both* the driver and motion layers; layer-specific types (e.g. trajectories) live in those layers.

### Key Design Patterns

**Single-target foundation:**
- `xmBase` is one STATIC library aggregating the telemetry API + the common types; consumers `find_package(xmBase)` and link `xmotion::xmBase`.
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

**C++ Workflow** (`.github/workflows/cpp.yml`): ubuntu-22.04 + ubuntu-24.04; separate build and test jobs (`-DBUILD_TESTING=ON`); sanitizer lanes (ASan/UBSan, TSan).

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
- Test files live in the top-level `test/` directory (e.g. `test/test_logging.cpp` — the console-binding suite; `test/test_telemetry_api.cpp` — the seam/ABI contract)
- Add new tests to `test/CMakeLists.txt`
- The console binding's concurrency test runs under TSan in CI; the telemetry suites run under ASan/UBSan
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
- Exports a single CMake target, `xmotion::xmBase` (via `find_package(xmBase)`)
- Includes CMake config files for find_package() support

## ROS Integration

The library can be built with colcon in a ROS workspace: place in `src/` and run `colcon build`. Time mapping to ROS clocks happens at the application boundary, not in this library.
