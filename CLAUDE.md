# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

xmotion-core is a C++ library for robot motion planning and control. It provides:
- **Interface definitions**: Hardware driver interfaces (motors, sensors, CAN, serial, etc.) and control interfaces
- **Logging utilities**: spdlog-based logging system with environment variable configuration
- **Type definitions**: Common geometry and trajectory types for robotics applications

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

**Run single test:**
```bash
# Build first
cmake --build . --config Release
# Run specific test
./bin/test_xlogger
./bin/test_logging
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

The codebase is organized into two main modules under `src/`:

1. **interface/**: Header-only interface definitions
   - `interface/type/`: Base types, geometry types, trajectory types
   - `interface/driver/`: Hardware driver interfaces (motor controllers, sensors, CAN, serial, joystick, etc.)
   - `interface/control/`: Controller interface template
   - All interfaces use pure virtual classes to define contracts

2. **logging/**: Logging implementation using spdlog
   - `logging/xlogger.hpp`: Main logging macros (XLOG_INFO, XLOG_DEBUG, etc.)
   - `logging/ctrl_logger.hpp`: Control-specific logger
   - `logging/csv_logger.hpp`: CSV file logger
   - Supports both printf-style and stream-style logging

### Key Design Patterns

**Interface Module:**
- All driver interfaces are pure virtual base classes in the `xmotion` namespace
- Interfaces define contracts; implementations throw `std::runtime_error` for unsupported operations
- Template-based controller interface allows type-safe state/output definitions
- Header-only library linked as INTERFACE target

**Logging Module:**
- Macro-based logging API that compiles out when `ENABLE_LOGGING` is disabled
- Singleton pattern for DefaultLogger
- Environment variable configuration (see below)
- Thread-safe logging with spdlog backend

### Namespace Convention

All code uses the `xmotion` namespace.

## Logging Configuration

The logging system is controlled via environment variables:

- `XLOG_LEVEL`: Log level (default: 2)
  - 0: Trace, 1: Debug, 2: Info, 3: Warn, 4: Error, 5: Fatal, 6: Off
- `XLOG_ENABLE_LOGFILE`: Enable file logging (TRUE/true/1 to enable, default: false)
- `XLOG_FOLDER`: Log file directory (default: ~/.xmotion/log)

**Usage in code:**
```cpp
#include "logging/xlogger.hpp"

// Printf-style
XLOG_INFO("Motor speed: %d RPM", speed);

// Stream-style
XLOG_DEBUG_STREAM("Position: " << x << ", " << y);
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

### Adding New Interfaces

When adding new driver or controller interfaces:
1. Create header-only interface in `src/interface/include/interface/driver/` or `interface/control/`
2. Use pure virtual methods with `= 0` for required operations
3. Provide default implementations that throw `std::runtime_error` for optional operations
4. Document which functions are required vs. optional in header comments
5. No need to modify CMakeLists.txt for header-only additions

### Testing

- Tests use GoogleTest (bundled in `third_party/googletest`)
- Test files located in module `test/` directories (e.g., `src/logging/test/`)
- Add new tests to module's `test/CMakeLists.txt`
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
- Package name: libxmotion-core
- Default install prefix: /opt/xmotion
- Exports CMake targets as `xmotion::interface` and `xmotion::logging`
- Includes CMake config files for find_package() support

## ROS Integration

The library detects ROS environments and adjusts accordingly:
- Automatically uses system spdlog when `ROS_DISTRO=humble`
- Can be built with colcon in a ROS workspace
- Place in `src/` directory of colcon workspace and run `colcon build`
