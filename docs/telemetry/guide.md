# xmBase Telemetry — Instrumentation Guide

How to instrument XMotion code with the telemetry API: logging, metrics, traces, signals, health. One header for everything:

```cpp
#include "xmbase/telemetry/telemetry.hpp"
namespace tel = xmotion::telemetry;
```

Companions: [blueprint.md](blueprint.md) (the whole module: tiers, data flow, roadmap) · [reference.md](reference.md) (every symbol + contract) · [design.md](design.md) (why it's shaped this way) · runnable [`examples/`](../../examples/).

## The two rules that matter

1. **Register at init, touch on the hot path.** `GetCounter`/`GetGauge`/`GetHistogram`/`GetChannel`/`GetEventSource` may allocate — call them once, keep the (cheap, copyable) handles, never look up by name in a loop.
2. **Names and format strings are string literals.** Backends intern by pointer. Dynamic text goes in *arguments* (`XM_INFO("state: {}", str)`) or the stream forms.

## Logging (was `XLOG_*`)

```cpp
XM_INFO("motor {} spun up at {:.2f} A", id, amps);   // fmt {} syntax, full spec support
XM_WARN_STREAM("pose " << pose);                     // ostream form (non-RT; lazily gated)
tel::SetLogLevel(tel::Severity::kDebug);             // runtime level (was XM_LOG_LEVEL)
```

Migration from `XLOG_*` is mechanical: `XLOG_` → `XM_`; `XLOG_LEVEL(x)` → `tel::SetLogLevel(...)`; compile floor `XMBASE_ACTIVE_LEVEL` → `XM_TELEMETRY_LEVEL`. Console behavior is preserved by the built-in dependency-free binding (`XM_LOG_LEVEL` env honored; file logging moved to the SDK recording plane).

**Attribute your subsystem** — one tag per driver/module, so events are filterable per-device downstream:

```cpp
class ImuDriver {
  tel::EventSource src_ = tel::GetEventSource("imu.front");   // once
  ...
  XM_ERROR_SRC(src_, "checksum fail on frame {}", seq);
  XM_DEBUG_STREAM_SRC(src_, "raw: " << frame);
};
```

Guard genuinely expensive message construction yourself with the same gate the macros use:

```cpp
if (tel::ShouldLog(tel::Severity::kDebug)) { auto dump = ExpensiveDump(); XM_DEBUG("{}", dump); }
```

## Metrics

```cpp
class MotorController {
  // init-time
  tel::Counter   faults_  = tel::GetCounter("motor.left.faults");
  tel::Gauge     temp_c_  = tel::GetGauge("motor.left.temp_c");
  tel::Histogram lat_us_  = tel::GetHistogram("motor.left.cmd_latency_us");

  void ControlStep() {                       // hot path — atomics only
    const auto t0 = tel::Now();
    ...
    if (fault) faults_.Add();
    temp_c_.Set(ReadTemp());
    lat_us_.Record(std::chrono::duration<double, std::micro>(tel::Now() - t0).count());
  }
};
```

Pick the right instrument: **Counter** = things that only accumulate (faults, retries, drops); **Gauge** = current value (temperature, queue depth, battery); **Histogram** = distributions (latency, jitter). Keep names low-cardinality — no per-message ids in names.

## Traces

Mint one trace per unit of work at **ingress**; scope each **stage**; carry identity across boundaries in the **envelope**:

```cpp
// ingress — one trace per planning iteration / command / request
tel::ContextGuard g(tel::NewTrace());        // set + auto-restore; NEVER bare SetCurrentContext

{
  XM_SPAN("plan.iteration");                // spans auto-nest; events inside carry span identity
  { XM_SPAN("plan.global_search"); ... }
  { XM_SPAN("plan.traj_generate"); ... }
}
```

Across a thread / queue / process boundary:

```cpp
msg.ctx = tel::Inject(tel::CurrentContext());                  // producer
tel::ContextGuard g(tel::Extract(msg.ctx.data(), msg.ctx.size()));  // consumer — validate!
```

Fan-in stages **link** their inputs instead of reparenting (an OTel span link — "consumed", not "caused by"):

```cpp
tel::Span gather("plan.gather_inputs");
for (auto& in : inputs) gather.AddLink(tel::Extract(in.ctx.data(), in.ctx.size()));
// single-input shorthand: XM_SPAN_LINKED("stage", upstream_ctx);
```

## Signals (high-rate recording)

For data you want in the flight recorder at full rate — controller state, setpoints vs. actuals — not in the metrics pipeline:

```cpp
struct ControlState { double setpoint, measured, command; };   // trivially copyable POD
tel::SignalChannel<ControlState> ch_ = tel::GetChannel<ControlState>("ctrl.state");  // init
ch_.Publish({sp, y, u});                                       // per cycle
```

Payloads are fixed-size (≤ `kMaxSignalPayload`); variable-length data decomposes into per-sample records (publish trajectory *points*, not a trajectory blob).

## Health

Report **transitions** with hysteresis — never steady-state spam, never flapping:

```cpp
if (next_state != state_) {   // and only after the condition held for N ticks
  state_ = next_state;
  tel::ReportHealth("imu.front", state_, "stale sample stream");
}
```

## What happens to the data (binding states)

Call sites never change; the backend decides where records go:

| Verb | Default (built-in console binding, zero init) | With the xmTelemetry SDK bound |
|---|---|---|
| events | console, like classic XLOG | captured + recorded/exported |
| metrics | aggregate in-slot (not yet exported) | sampled + exported (OTLP → Grafana) |
| scopes/links | dropped | trace records (OTLP/Foxglove) |
| signals | dropped | MCAP flight recorder |
| health | logged as `[health] …` lines | attributed records + health metrics |
| lib-only, `ENABLE_LOGGING=OFF` | events ≥ Warn → stderr; rest no-op | n/a |

Rule of thumb: **libraries instrument, applications bind.** A library never calls `Init`/`InstallBinding`; whoever owns `main()` does.

## RT checklist (control loops)

- ✅ handles + `EventSource` acquired before the loop starts
- ✅ only `Add`/`Set`/`Record`/`Publish`/`XM_*` (fmt form)/`XM_SPAN` inside the loop
- ❌ no `Get*` / `NewTrace` / `_STREAM` forms / string building on the hot path
- ✅ dynamic strings pre-truncate: event args cap at 8 args / 160 B pack (see reference.md)
