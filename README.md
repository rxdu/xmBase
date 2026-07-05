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
- **Logging** — **one API with telemetry**: the `XM_*` macros (`XM_INFO`, `XM_WARN_STREAM`, …) ARE the telemetry `event()` verb (the former `XLOG_*` spelling is gone — clean break). They are backed by a built-in, dependency-free console binding (synchronous, thread-safe, `XM_LOG_LEVEL` env config); when an application binds the xmTelemetry SDK, the same call sites gain RT-safe capture, flight recording, and export — with zero code changes.
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
| `include/xmbase/telemetry/`      | the instrumentation API: logging macros (`XM_*`, soft-RT via the event() verb), metric/scope/signal verbs, context spine, binding seam |
| `include/xmbase/types/`          | header-only common types: `base_types.hpp`, `geometry_types.hpp`            |
| `src/`                            | the compiled telemetry bindings (console default + unbound fallback)        |

## Build

```bash
mkdir build && cd build
cmake ..
make -j
```

Key options: `BUILD_TESTING` (build tests, default `OFF`), `ENABLE_LOGGING` (default `ON`),


## Telemetry & logging

Full documentation lives in [`docs/telemetry/`](docs/telemetry/): **[blueprint](docs/telemetry/blueprint.md)** (the whole module: tiers, data flow, roadmap, coverage matrix) · **[guide](docs/telemetry/guide.md)** (how to instrument: logging, metrics, traces, signals, health) · **[reference](docs/telemetry/reference.md)** (every symbol + contract) · **[design](docs/telemetry/design.md)** (the API tier and the binding seam) · **[comparison](docs/telemetry/comparison.md)** (robotics application coverage + OpenTelemetry comparison). Runnable reference examples: the [examples tour](examples/README.md) — hot loop (S1), trace pipeline (S2), worker-pool context discipline (S2-A5), device health (S3), the executable seam/blueprint, logging, types. The executable specification is the [xmTelemetry scenario suite](https://github.com/rxdu/xmTelemetry/blob/main/docs/scenarios.md).

One front-end (format strings use fmt `{}` syntax, not printf):

```cpp
#include "xmbase/telemetry/telemetry.hpp"  // the one instrumentation header
XM_INFO("motor speed: {} RPM", speed);
XM_WARN_STREAM("temp " << t << " C");
```

A dedicated hard-RT path (the lock-free ring that will become the xmTelemetry SDK's capture
channel) exists as private implementation under `src/logging/` and is CI-tested; it re-emerges
through the SDK so that `XM_*` itself is RT-safe. See [docs/logging.md](docs/logging.md).

### Environment configuration

* `XM_LOG_LEVEL`: 0–6 (0: TRACE, 1: DEBUG, 2: INFO, 3: WARN, 4: ERROR, 5: FATAL, 6: OFF)

## License

Apache-2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE). First-party code only; bundled
third-party components retain their own licenses.
