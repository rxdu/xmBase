# XMotion Telemetry — Module Blueprint

The map of the telemetry module from the user's side: the API tier in this repo is the part you touch, and this page shows what you get with xmBase alone, what changes when an application binds the xmTelemetry SDK, and where everything is documented.

Companions: [design.md](design.md) (API-tier design) · [reference.md](reference.md) (symbol contracts) · [guide.md](guide.md) (how to instrument) · [comparison.md](comparison.md) (application coverage + OpenTelemetry comparison) · [ADR 0004](https://github.com/rxdu/xmotion/blob/main/docs/adr/0004-telemetry-layering.md) (layering rationale).

## 1. The layering

```
  xmDriver / xmNavigation / application code           1 kHz control loop
        │                                                    │ (RT subset)
        ▼                                                    ▼
┌────────────── API  (xmBase, include/xmbase/telemetry/) ──────────────────┐
│  event()   XM_*[_SRC][_STREAM]      — deferred-format ArgPack            │
│  metric()  Counter/Gauge/Histogram  — atomics on registration-fixed slots│
│  scope()   XM_SCOPE / AddLink       — RAII spans, nesting, span links    │
│  signal()  SignalChannel<T>         — fixed-size POD samples             │
│  health()  ReportHealth             — transition convention              │
│  context spine: NewTrace · ContextGuard · Inject/Extract   ONE clock     │
└──────────────┬────────────────────────────────────────────────────────── ┘
               │ BINDING SEAM (binding.hpp): install-once fn table, ABI-gated
               │   default: the built-in dependency-free console binding
               ▼
   xmTelemetry SDK — the production observability runtime (private)
```

Dependency rule: components instrument against the **API only**; applications choose the backend. `xmBase` never depends on xmTelemetry; neither depends on ROS.

## 2. What you get with xmBase alone

The free-standing experience is complete and honest:

- **Full-severity console logging** through the built-in binding — dependency-free (libc/libstdc++ only), synchronous, thread-safe, colorized on a tty, `XLOG_LEVEL` env seeding, deferred-format `{}` events rendered with the documented spec subset.
- **Safe everything else** — metric handles, scopes, and signal channels are valid no-ops; handles stay safe to hold for the whole process lifetime.
- **The seam is yours too** — `binding.hpp` is a public, ABI-gated contract; `examples/custom_binding_example.cpp` shows a complete third-party backend.

## 3. What the xmTelemetry SDK adds

xmTelemetry is the privately maintained production runtime behind the same call sites — binding it requires **zero changes to instrumented code**. At the capability level:

- **Crash-surviving flight recording** — the seconds before a `SIGKILL` or panic are recoverable post-mortem, with the log tail, spans, and metric values intact, on an unmodified machine.
- **A recording plane** — MCAP recordings that open directly in Foxglove/PlotJuggler, with rotation, retention, storage-fault tolerance, and pre/post-trigger snapshot windows.
- **Insight tooling** — live tailing of a running process, one-command post-mortem triage reports that surface sporadic stalls and rare bugs with trace drill-downs, Perfetto/OTLP exports, and machine-readable digests that gate CI on performance regressions.
- **RT discipline throughout** — wait-free capture, bounded memory, counted drops, sanitizer-verified, with enforced per-operation latency budgets.

*xmTelemetry is available for production integrations — reach out via the repository owner.*

## 4. Where everything lives

| Piece | Where | Status |
|---|---|---|
| API tier (verbs, spine, seam, console binding) | **xmBase** `include/xmbase/telemetry/`, `src/telemetry_*.cpp` | ✅ shipped |
| API docs (design/reference/guide/comparison) | xmBase `docs/telemetry/` | ✅ |
| Runnable examples (hot loop, trace pipeline, device health, custom binding, …) | xmBase `examples/` | ✅ |
| Layering rationale | umbrella `docs/adr/0004…` | ✅ |
| SDK, recording plane, insight tools | **xmTelemetry** (private) | ✅ shipped, production-tested |

## 5. Life of a record

1. **Emit** — a verb call on the hot path: metric ⇒ atomic op on its pre-registered slot; event/span/signal ⇒ bounded stack copy + one seam call. Suppressed severities exit at the `should_log` gate before any packing.
2. **Backend** — the seam call lands in whichever binding is installed: the console binding renders events to stderr; the SDK captures everything per its guarantees; a custom binding does whatever you built.

That is the whole public contract: everything past the seam is the binding's business, and call sites never change when the backend does.
