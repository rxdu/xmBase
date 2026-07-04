# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

xmBase is the foundation library of the XMotion family — the shared substrate every other component builds on. Because it is depended on by *all* components, it deliberately contains only what is universal across the whole family:
- **Logging utilities**: spdlog-based logging system with environment variable configuration
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
ctest -R XLoggerTest --output-on-failure
```

### CMake Options

- `BUILD_TESTING`: Build tests (default: OFF)
- `XMOTION_DEV_MODE`: Development mode, forces building tests (default: OFF)
- `ENABLE_LOGGING`: Enable spdlog logging (default: ON)
- `USE_SYS_SPDLOG`: Use system spdlog instead of bundled (default: ON)
- `STATIC_CHECK`: Enable cppcheck static analysis (default: OFF)

### Dependencies

**Required:**
- CMake >= 3.10.2
- C++17 compiler
- Eigen3
- libspdlog-dev (if USE_SYS_SPDLOG=ON)

**Install on Ubuntu:**
```bash
sudo apt-get install libeigen3-dev libspdlog-dev
```

## Architecture

### Module Structure

Everything compiles into one target, `xmotion::xmBase`. Headers live under
`include/xmbase/`; the compiled logging sources under `src/`.

1. **logging/** (`include/xmbase/logging/`): logging front-ends; the spdlog-backed
   implementation is the compiled part under `src/`.
   - `xmbase/telemetry/event.hpp`: the logging macros (`XM_INFO`, `XM_DEBUG`, …; stream-style `XM_*_STREAM`) — the telemetry event() verb, interim spdlog backend. fmt `{}` syntax.
   - `rt_logger.hpp` / `rt_logger_mpsc.hpp`: hard-RT macros (`XLOG_RT_*`) — lock-free, allocation-free `RtLogger` (single-producer) and `MpscRtLogger` (multi-producer).
   - `ctrl_logger.hpp`: control-specific logger; `csv_logger.hpp`: CSV file logger; `event_logger.hpp`: structured event logger.

2. **types/** (`include/xmbase/types/`): Header-only common type vocabulary (namespace `xmotion`)
   - Granular headers: `scalar.hpp` (enum base), `time.hpp` (`Clock`/`Timestamp`/`Duration`), `vector.hpp` (POD `vector3_t`/`vector4_t` for the wire/driver layer), `geometry.hpp` (Eigen-backed pose/velocity/joint/wrench + `Pose`/`Twist`/`Odometry`), `stamped.hpp` (`Stamped<T>`).
   - `types.hpp`: umbrella that pulls in all of the above.
   - `quantities.hpp`: **opt-in** strong-typed quantities (tagged `Vec3`: `Force`, `Torque`, `LinearVelocity`, …) — distinct types so the compiler catches quantity mix-ups; reach Eigen via `.vec()`. Not included by the umbrella.
   - `base_types.hpp` / `geometry_types.hpp`: compatibility facades re-exporting the granular headers (legacy include paths used by xmDriver/xmNavigation).
   - Conventions: SI units, radians, intrinsic Z-Y-X Euler, Hamilton quaternions; all composite types default-initialize to identity/zero.
   - These are the types shared by *both* the driver and motion layers; layer-specific types (e.g. trajectories) live in those layers.

### Key Design Patterns

**Single-target foundation:**
- `xmBase` is one STATIC library aggregating logging + the common types; consumers `find_package(xmBase)` and link `xmotion::xmBase`.
- There are intentionally no driver/control interfaces here — they belong to xmDriver's HAL (`xmmu/hal/`). Keeping xmBase free of upper-layer specifics is a load-bearing design rule, not an accident.

**Telemetry API (xmbase/telemetry/, ADR 0004):** the stateless XMotion instrumentation
surface — 4 verbs (`event`/`metric`/`scope`/`signal`) + health, context spine
(TraceId/Context/NewTrace/Inject/Extract), install-once binding seam (`binding.hpp`). All
machinery lives in the optional xmTelemetry SDK. Unbound fallback: events >= Warn and
non-Ok health go to stderr; everything else no-ops.

**xmbase/logging/ is transitional (departure map):** every resident leaves xmBase per
ADR 0004 — `details/`+spdlog backend -> replaced by the SDK ConsoleSink (P0b/P1);
`rt_logger*` -> its Vyukov ring becomes the SDK capture channel (P0b), then `XLOG_RT_*`
dissolves into RT-safe `XM_*`; `csv/ctrl/event_logger` (zero in-family consumers) ->
deprecated, superseded by `signal()`/`event()` + McapSink (P2/P3). End state: the folder is
deleted; xmBase = `telemetry/` (API) + `types/` + containers. Do NOT move machinery under
`xmbase/telemetry/` — that directory is the stateless API tier by definition.

**Logging Module (ONE API with telemetry):**
- The `XM_*` macros ARE the logging front-end (the former `XLOG_*` spelling was removed — clean break); backed
  today by the interim spdlog binding (`src/telemetry_logging_binding.cpp`), replaced
  wholesale when an application installs the xmTelemetry SDK binding.
- Macro-based logging API that compiles out when `ENABLE_LOGGING` is disabled; the
  compile-time `XMBASE_ACTIVE_LEVEL` floor strips below-floor call sites entirely.
- **Soft-RT default (`XM_*`):** async spdlog (caller formats + enqueues, a worker thread
  does the I/O); a full queue drops the oldest record rather than blocking. Singleton
  `DefaultLogger`. Use for everything without a hard deadline.
- **Hard-RT (`XLOG_RT_*`):** a per-loop `RtLogger`/`MpscRtLogger` over a lock-free ring drained
  by a background thread — wait-free/lock-free, no heap, no syscall, never blocks; a full ring
  drops the newest record and bumps `dropped()`. Honors `XLOG_LEVEL`; `SetLevel()` at runtime.
- Environment variable configuration (see below); thread-safe with spdlog backend.

### Namespace Convention

All code uses the `xmotion` namespace.

## Logging Configuration

The logging system is controlled via environment variables:

- `XLOG_LEVEL`: Log level (default: 2)
  - 0: Trace, 1: Debug, 2: Info, 3: Warn, 4: Error, 5: Fatal, 6: Off
- `XLOG_ENABLE_LOGFILE`: Enable file logging (TRUE/true/1 to enable, default: false)
- `XLOG_FOLDER`: Log file directory (default: ~/.xmotion/log)

**Usage in code** (format strings use fmt `{}` syntax, **not** printf):
```cpp
#include "xmbase/telemetry/telemetry.hpp"

// fmt-style (soft-RT, async)
XM_INFO("Motor speed: {} RPM", speed);

// Stream-style
XM_DEBUG_STREAM("Position: " << x << ", " << y);
```

For a hard-real-time loop, use the `XLOG_RT_*` front-end instead:
```cpp
#include "xmbase/logging/rt_logger.hpp"

xmotion::RtLogger rt("ctrl_loop");          // one logger owned by the loop
XLOG_RT_INFO(rt, "cycle {} tau={:.3f}", cycle, tau);  // wait-free, no heap/syscall
rt.Flush();                                 // NON-RT: drain before exit/shutdown
```

**Important:** Do not make logging calls after a signal is received (undefined behavior).

## CI/CD

### GitHub Actions Workflows

**C++ Workflow** (`.github/workflows/cpp.yml`):
- Runs on: ubuntu-22.04, ubuntu-24.04
- Separate build and test jobs
- Tests use `-DBUILD_TESTING=ON`

**ROS Workflow** (`.github/workflows/ros.yml`):
- Builds with ROS Humble and Jazzy
- Uses colcon build system
- Automatically sets `USE_SYS_SPDLOG=ON` for ROS Humble

## Development Guidelines

### Code Style

- **C++ Standard**: C++17 (set in CMakeLists.txt)
- **File Extensions**: `.cpp` for source, `.hpp` for headers
- Follow existing naming conventions in the codebase
- Use clang-format with Google style for formatting

### What belongs here (and what doesn't)

Add to xmBase only things every component could share: logging facilities, or a type that is genuinely spoken by more than one layer. Concretely:
- A new **common type** goes in `include/xmbase/types/` (header-only; no CMakeLists change needed).
- A new **driver/control interface** does **not** go here — it belongs to its owning component (driver interfaces → xmDriver's `xmmu/hal/`; control/motion types → xmNavigation). If a would-be "common" type is only used by one upper layer, put it in that layer instead.

### Testing

- Tests use GoogleTest (bundled in `third_party/googletest`)
- Test files live in the top-level `test/` directory (e.g. `test/test_rt_logger.cpp`)
- Add new tests to `test/CMakeLists.txt`
- The lock-free RT loggers carry property-based tests run under TSan + ASan/UBSan in CI
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

The library detects ROS environments and adjusts accordingly:
- Automatically uses system spdlog when `ROS_DISTRO=humble`
- Can be built with colcon in a ROS workspace
- Place in `src/` directory of colcon workspace and run `colcon build`
