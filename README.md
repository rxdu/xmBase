<h1 align="center">
  <img src="docs/xmbase.svg" width="96" alt="xmBase"><br>
  xmBase&nbsp;·&nbsp;Σ
</h1>

<p align="center"><b>Foundation layer for the XMotion family</b> — logging and common types.<br>
The substrate every other component plugs into.</p>

---

`xmBase` is the foundation of the **XMotion** product family. It provides the low-level
utilities shared across components:

- **Telemetry API** — the stateless XMotion instrumentation surface ([ADR 0004](https://github.com/rxdu/xmotion/blob/main/docs/adr/0004-telemetry-layering.md)): four verbs (`event`/`metric`/`scope`/`signal`) + health, callable from a 1 kHz control loop; all machinery lives in the optional [xmTelemetry](https://github.com/rxdu/xmTelemetry) SDK, bound at runtime.
- **Logging** — **one API with telemetry**: `XLOG_*` is a facade over the telemetry `event()` verb. Today it is backed by the interim spdlog binding in this repo (async soft-RT, env-var config, log files — behavior unchanged); when an application links the xmTelemetry SDK, the same call sites flow through its rings/sinks. `XLOG_RT_*` (lock-free hard-RT) remains separate until the SDK absorbs its ring (P0b).
- **Common types** — the shared geometry/primitive type vocabulary (`xmbase/types/`, namespace `xmotion`) spoken by both the driver layer (xmDriver) and the motion layer (xmNavigation).

> Driver/control interfaces are intentionally **not** here — they belong to their owning
> component (xmDriver's HAL). Keeping xmBase free of upper-layer specifics is a load-bearing design rule.

It builds either standalone or embedded as a module in another project, and ships its own CI +
Debian packaging so downstream components can consume released artifacts rather than source.

> Part of the XMotion family — see the [umbrella](https://github.com/rxdu/xmotion). Sibling
> components include [xmNavigation](https://github.com/rxdu/xmNavigation) (motion algorithms) and
> [xmDriver](https://github.com/rxdu/xmDriver) (host hardware drivers).

## Layout

Headers live under `include/xmbase/`; the compiled logging sources under `src/`. Everything
builds into one CMake target, `xmotion::xmBase`.

| Path                              | Description                                                                 |
|-----------------------------------|-----------------------------------------------------------------------------|
| `include/xmbase/logging/`        | logging front-ends: `xlogger` (`XLOG_*`, soft-RT), `rt_logger` / `rt_logger_mpsc` (`XLOG_RT_*`, hard-RT), plus `csv_logger`, `ctrl_logger`, `event_logger` |
| `include/xmbase/types/`          | header-only common types: `base_types.hpp`, `geometry_types.hpp`            |
| `src/`                            | spdlog-backed logging implementation (the compiled part)                    |

## Build

```bash
mkdir build && cd build
cmake ..
make -j
```

Key options: `BUILD_TESTING` (build tests, default `OFF`), `ENABLE_LOGGING` (default `ON`),
`USE_SYS_SPDLOG` (use system spdlog, default `ON`).

## Logging

Two front-ends, picked by deadline (format strings use fmt `{}` syntax, not printf):

```cpp
#include "xmbase/logging/xlogger.hpp"     // soft-RT (async)
XLOG_INFO("motor speed: {} RPM", speed);

#include "xmbase/logging/rt_logger.hpp"   // hard-RT (lock-free, drop-on-full)
xmotion::RtLogger rt("ctrl_loop");
XLOG_RT_INFO(rt, "cycle {} tau={:.3f}", cycle, tau);   // wait-free, no heap/syscall
rt.Flush();                                            // NON-RT: drain at shutdown
```

The hard-RT path also honors `XLOG_LEVEL` and can be retargeted at runtime via `rt.SetLevel(...)`.
See [docs/logging.md](docs/logging.md) for the full contract (drain-thread placement, timestamps).

### Environment configuration

* `XLOG_LEVEL`: 0–6 (0: TRACE, 1: DEBUG, 2: INFO, 3: WARN, 4: ERROR, 5: FATAL, 6: OFF)
* `XLOG_ENABLE_LOGFILE`: 0 or 1
* `XLOG_FOLDER`: folder for log files (default `~/.xmotion/log`)

## License

Apache-2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE). First-party code only; bundled
third-party components retain their own licenses.
