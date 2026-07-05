# xmBase Telemetry — API-Tier Design

- Status: Active (API tier shipped; SDK lands in xmTelemetry P0b)
- Governing decisions: [ADR 0004 — telemetry layering](https://github.com/rxdu/xmotion/blob/main/docs/adr/0004-telemetry-layering.md); full-stack design in the [umbrella design doc](https://github.com/rxdu/xmotion/blob/main/docs/design/telemetry-library-design.md)
- Executable specification: the [xmTelemetry scenario suite](https://github.com/rxdu/xmTelemetry/blob/main/docs/scenarios.md) (S1–S13)
- Companion docs: [blueprint.md](blueprint.md) (the WHOLE module: tiers, data flow, roadmap, coverage) · [reference.md](reference.md) (every symbol + contract) · [guide.md](guide.md) (how to instrument) · [`examples/`](../../examples/)

## 1. What this is

One instrumentation surface for the whole XMotion family — logs, metrics, causal traces, high-rate signals, and health — usable identically from a 1 kHz control loop and a planning thread. This document covers the **API tier**, which lives in xmBase and is the part users interact with; the machinery (rings, drain, sinks, exporters) is the xmTelemetry SDK's job and is deliberately invisible here.

The layering mirrors OpenTelemetry's API/SDK/exporter split, adapted for robotics (ADR 0004):

| Tier | Home | Contents | Linked |
|------|------|----------|--------|
| **API** | **xmBase** `include/xmbase/telemetry/` | 4 verbs + health, context spine, handles, binding seam, unbound fallback | always |
| SDK | xmTelemetry | rings, drain, router, metric sampling, Console/Null sinks, lifecycle | optional |
| Exporters | xmTelemetry (per CMake option) | MCAP flight recorder, OTLP, LTTng | opt-in |

Dependency rule: components (xmDriver, xmNavigation, …) instrument against **this API only**; applications choose the SDK and exporters. xmBase never depends on xmTelemetry; neither depends on ROS.

## 2. Design constraints the API answers

1. **RT-safe hot path** — every hot-path operation is `noexcept`, allocation-free, and bounded: metric updates are relaxed atomics on a pre-registered slot; events copy arguments into a fixed stack pack (formatting is deferred); scopes cost two clock reads + one id. Nothing on the hot path takes a lock or touches a sink.
2. **Stateless foundation** — the API tier holds no machinery: no threads, no buffers, no aggregation. Its only state is a thread-local `Context` (a POD) and one atomic binding pointer. That is what makes "xmTelemetry is optional" literally true.
3. **One timeline, one identity** — the time base *is* the family `xmotion::Clock/Timestamp` (never a parallel clock), and a `TraceId/SpanId` context propagates across threads and processes via `Inject`/`Extract`, so a control glitch, a log line, and a planning stall align by construction.
4. **Honest degraded mode** — with no SDK, faults are never silently swallowed: events ≥ Warn and non-Ok health go to stderr; everything else is a safe no-op.

## 3. The binding seam (`binding.hpp`)

The API↔SDK boundary is an **install-once table of function pointers** (`Binding`), guarded by `kBindingAbiVersion`. Design choices and why:

- **Install-once, not per-call lookup**: handles resolve to slot pointers at *registration*; the only per-call indirection on event/span/signal paths is one atomic acquire load of the binding pointer. No weak symbols (ODR/platform fragility), no OTel-style provider lookup (not RT-safe).
- **Slot memory is process-lifetime** by contract — the SDK never frees metric slots. Consequence: a handle held across `Shutdown()` stays *safe* (writes go to a dead-but-valid slot). This kills the static-destruction crash class that plagues logging libraries (scenario S7).
- **Names and format strings must be string literals** — the SDK interns by pointer identity; the deferred-format hot path never copies strings it doesn't have to.
- **Binding states**:

| State | When | events | metrics | scopes/signals |
|---|---|---|---|---|
| unbound | no SDK, or after explicit `InstallBinding(nullptr)` | ≥ Warn → stderr | no-op slots | no-op |
| **interim logging binding** | xmBase built with `ENABLE_LOGGING` (default) — auto-adopted on first use | classic logging: async spdlog, `XLOG_*` env vars, log files, full fmt fidelity | aggregate (unexported) | dropped |
| SDK bound | app called `xmotion::telemetry::Init()` (xmTelemetry) | SDK rings → sinks | sampled + exported | recorded/exported |

  An **explicit** `InstallBinding` call — including `nullptr` (the SDK's `Shutdown()` end-state) — is authoritative and disables auto-adoption for the rest of the process.

## 4. What is intentionally NOT here

- **No spans/metrics engine** — trace assembly, histogram export, OTLP encoding are SDK/exporter work.
- **No public logging module** — `XLOG_*` was unified into the `event()` verb (clean break, v0.3.0); the spdlog interim backend and the RT ring live as *private* implementation under `src/logging/` until they migrate into the SDK (P0b/P1). Nothing logging-shaped is installed.
- **No configuration surface** — env-var/config ownership moves to the SDK; the API only exposes `SetLogLevel`/`SetResource` pass-throughs.

## 5. Cost model (hot path, bound)

| Call | Cost |
|---|---|
| `counter.Add()` / `gauge.Set()` / `histogram.Record()` | relaxed atomic op(s) on a fixed slot; no binding load |
| `XM_INFO(...)` (below runtime level) | one binding load + one `should_log` call — args not even packed |
| `XM_INFO(...)` (enabled) | arg copy into a ≤160 B stack pack + one seam call (SDK: ring push) |
| `XM_SCOPE` | 2× `Now()` + 1 id generation + 1 seam call at destruction |
| `channel.Publish(pod)` | one seam call with a ≤`kMaxSignalPayload` byte copy |
| anything, unbound | the same minus the seam call (stderr only for Warn+/faults) |

## 6. How the API evolved (and evolves)

The surface was **scenario-driven**: realistic use cases were written as wish-code *before* the API existed, and every gap they exposed became a numbered delta (D1–D15 so far — trace creation, context guards, value handles, span links, pre-init contract, `Shutdown(deadline)`, …) recorded in the [scenario catalog](https://github.com/rxdu/xmTelemetry/blob/main/docs/scenarios.md). API changes continue to flow through that loop: propose via a scenario or real consumer friction → record the delta → change the surface (bump `kBindingAbiVersion` if the seam moves) → update the scenarios in lockstep.
