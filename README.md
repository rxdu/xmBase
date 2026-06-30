<h1 align="center">
  <img src="docs/xmsigma.svg" width="96" alt="xmSigma"><br>
  xmSigma&nbsp;·&nbsp;Σ
</h1>

<p align="center"><b>Foundation layer for the xMotion family</b> — logging and common types.<br>
The substrate every other component plugs into.</p>

---

`xmSigma` is the Σ foundation of the **xMotion** product family. It provides the low-level
utilities shared across components:

- **Logging** — a dual-mode logging system on an spdlog backend: an async **soft-RT** front-end (`XLOG_*`) for general use, and a lock-free, allocation-free **hard-RT** front-end (`XLOG_RT_*`) for control loops with a hard deadline. Configurable via environment variables.
- **Common types** — the shared geometry/primitive type vocabulary (`xmsigma/types/`, namespace `xmotion`) spoken by both the driver layer (xmMu) and the motion layer (xmNabla).

> Driver/control interfaces are intentionally **not** here — they belong to their owning
> component (xmMu's HAL). Keeping Σ free of upper-layer specifics is a load-bearing design rule.

It builds either standalone or embedded as a module in another project, and ships its own CI +
Debian packaging so downstream components can consume released artifacts rather than source.

> Part of the xMotion family — see the [umbrella](https://github.com/rxdu/xmotion). Sibling
> components include [xmNabla](https://github.com/rxdu/xmNabla) (motion algorithms) and
> [xmMu](https://github.com/rxdu/xmMu) (host hardware drivers).

## Layout

Headers live under `include/xmsigma/`; the compiled logging sources under `src/`. Everything
builds into one CMake target, `xmotion::xmSigma`.

| Path                              | Description                                                                 |
|-----------------------------------|-----------------------------------------------------------------------------|
| `include/xmsigma/logging/`        | logging front-ends: `xlogger` (`XLOG_*`, soft-RT), `rt_logger` / `rt_logger_mpsc` (`XLOG_RT_*`, hard-RT), plus `csv_logger`, `ctrl_logger`, `event_logger` |
| `include/xmsigma/types/`          | header-only common types: `base_types.hpp`, `geometry_types.hpp`            |
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
#include "xmsigma/logging/xlogger.hpp"     // soft-RT (async)
XLOG_INFO("motor speed: {} RPM", speed);

#include "xmsigma/logging/rt_logger.hpp"   // hard-RT (lock-free, drop-on-full)
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
