# XMotion Telemetry — Module Blueprint

The complete picture of the telemetry module — every tier, the data flow end to end, what exists today vs. what lands per phase, and where every piece is specified, documented, and demonstrated. The API tier (this repo) is the part users touch; this document is the map of everything behind it.

Companions: [design.md](design.md) (API-tier design) · [reference.md](reference.md) (symbol contracts) · [guide.md](guide.md) (how to instrument) · [comparison.md](comparison.md) (application coverage + OpenTelemetry comparison) · [ADR 0004](https://github.com/rxdu/xmotion/blob/main/docs/adr/0004-telemetry-layering.md) + [umbrella design doc](https://github.com/rxdu/xmotion/blob/main/docs/design/telemetry-library-design.md) (full-stack rationale) · [scenario suite](https://github.com/rxdu/xmTelemetry/blob/main/docs/scenarios.md) (executable spec).

## 1. The full stack

```
  xmDriver / xmNavigation / application code            1 kHz control loop
        │                                                     │ (RT subset)
        ▼                                                     ▼
┌────────────── TIER 1 · API  (xmBase, include/xmbase/telemetry/) ────────────────┐
│  event()   XM_*[_SRC][_STREAM]      — deferred-format ArgPack                    │
│  metric()  Counter/Gauge/Histogram  — atomics on registration-fixed slots        │
│  scope()   XM_SCOPE / AddLink       — RAII spans, nesting, span links            │
│  signal()  SignalChannel<T>         — fixed-size POD samples                     │
│  health()  ReportHealth             — transition convention                      │
│  context spine: NewTrace · ContextGuard · Inject/Extract   time base: ONE clock  │
└──────────────┬────────────────────────────────────────────────────────────────── ┘
               │ BINDING SEAM (binding.hpp): install-once fn table, ABI-gated
               │   today: interim spdlog binding (auto-adopted) — logging only
               ▼
┌────────────── TIER 2 · SDK  (xmTelemetry, P0b) ──────────────────────────────────┐
│  Init(SdkConfig) / Shutdown(deadline)   pre-init buffer (D9)   registry caps (D13)│
│  CHANNELS: heap ring │ mmap BLACK BOX (crash-survivable, P2) │ LTTng UST (P5)     │
│  per-QoS-class rings (diagnostics ≠ signals) · drop-newest + counted             │
│  metric aggregation (drain-sampled) · DRAIN thread · ROUTER · Null/Console sinks │
└──────┬─────────────────────────────┬───────────────────────────┬─────────────────┘
       │ diagnostics                 │ raw signals               │ (channel, not sink)
┌──────▼───────┐            ┌────────▼────────┐          ┌───────▼────────┐
│ OtelSink     │            │ McapSink        │          │ LTTng UST      │  TIER 3 ·
│ → OTLP       │            │ flight recorder │          │ kernel-corr.   │  EXPORTERS
│ → per-host   │            │ + xmtelemetry-  │          │ tracing        │  (per CMake
│   Collector  │            │   recover CLI   │          │                │   option)
└──────┬───────┘            └────────┬────────┘          └────────────────┘
       ▼                             ▼
  Grafana / Tempo              Foxglove / offline analysis
```

Dependency rule: components → **API only**; applications choose the SDK + exporters. `xmBase` never depends on xmTelemetry; neither depends on ROS (any ROS glue is an app-side bridge).

## 2. Where everything lives

| Piece | Repo · path | Status |
|---|---|---|
| API tier (verbs, spine, seam, unbound fallback) | **xmBase** `include/xmbase/telemetry/` | ✅ shipped (v0.3.0) |
| Interim spdlog binding (classic logging behavior) | xmBase `src/telemetry_logging_binding.cpp` (private) | ✅ transitional — replaced at P0b |
| RT ring (SDK capture-channel donor) + property tests | xmBase `src/logging/` (private) | ✅ donor, migrates at P0b |
| Scenario suite (executable spec, S1–S13) | **xmTelemetry** `docs/scenarios.md`, `test/scenarios/` | ✅ S6 runs; others armed at P0b |
| SDK surface stubs (`SdkConfig`, `Init/Shutdown`, test hooks) | xmTelemetry `include/xmtelemetry/` | ✅ declarations (the P0b contract) |
| SDK implementation (rings, drain, router, sinks) | xmTelemetry `sdk/` | ⏳ P0b |
| mmap black box + `xmtelemetry-recover` | xmTelemetry `blackbox/` | ⏳ P2 |
| MCAP / OTLP exporters, host collectors, LTTng channel | xmTelemetry, one CMake option each | ⏳ P3–P5 |
| Full-stack rationale (three-plane model, engine adoption) | umbrella `docs/adr/0004…`, `docs/design/…`, `docs/research/…` | ✅ |

## 3. Life of a record (data flow)

1. **Emit** — a verb call on the hot path: metric ⇒ atomic op on its slot (no seam call); event/span/signal ⇒ bounded POD (ArgPack / span record / payload copy) + one seam call. Suppressed severities exit at the `should_log` gate before packing.
2. **Capture** (SDK) — the seam call pushes into the QoS-class ring (wait-free, drop-newest + counted). The black-box channel is the same ring backed by a tmpfs mmap, so the last N seconds survive a crash.
3. **Drain** (SDK, non-RT thread) — pops records, samples metric aggregates on a period, timestamps stay the emit-time stamps.
4. **Route** — diagnostics (events/metric samples/spans/health) → OTel-path sinks; raw signals → recording-plane sinks. The router enforces this so a 1 kHz stream never floods the metrics pipeline.
5. **Export** — MCAP files (flight recorder, snapshot-on-trigger), OTLP → one per-host Collector (store-and-forward for intermittent connectivity), optional LTTng for kernel correlation.
6. **Recover** — after a crash, `xmtelemetry-recover` reads the mmap alone (genesis block makes it decodable on a clean machine) and emits MCAP.

Today, with only the interim binding: step 1 works fully; events take the classic spdlog path; metrics aggregate unexported; spans/signals stop at the seam. Every record lights up as later phases land — call sites never change.

## 4. Roadmap and gates

| Phase | Delivers | Gate (scenarios) |
|---|---|---|
| **P0a — done (v0.3.0)** | API tier, seam v2 (span links), XLOG unification, docs package | wish-code compiles; **S6** passes |
| **P0b** | SDK skeleton: Init/Shutdown, handle table, rings (donated RT ring), drain, router, Capture/Console sinks | **S1 S2 S3 S5 S7** behavioral — *opens the xmNavigation refactor* |
| P1 | RT hardening + benchmarks (alloc-free proof, overhead budget as CI artifact); spdlog leaves xmBase | S8 fork, S9 soak, S10 clocks |
| P2 | mmap black box + recover CLI (genesis block, sync markers) | **S4** crash recovery; S11/S12 partial |
| P3 | McapSink flight recorder; OtelSink → Collector; recording-plane fault injection | S11 S12 S13 |
| P4–P5 | host collectors; LTTng channel; app-side ROS bridge | — |

## 5. Coverage matrix

Every verb/pattern, and where it is specified, documented, and demonstrated:

| Pattern | Guide § | Reference § | Runnable example (this repo) | Scenario (executable spec) |
|---|---|---|---|---|
| Logging / events (+ attribution) | Logging | Events | all examples; `device_health_example` (attribution) | S6, S7 |
| Metrics (counter/gauge/histogram) | Metrics | Metrics | `instrumented_control_loop_example` | S1, S9 |
| Traces: nesting + propagation + links | Traces | Traces | `trace_pipeline_example` | S2 |
| Signals (+ per-sample decomposition, D5) | Signals | Metrics→SignalChannel | `custom_binding_example` (trajectory) | S1, S5 |
| Health transitions + hysteresis | Health | Health | `device_health_example` | S3 |
| RT hot-loop discipline | RT checklist | Time/Metrics | `instrumented_control_loop_example` | S1, S5 |
| The seam / writing a backend | — | The seam | **`custom_binding_example`** | S7 (lifecycle), fake-SDK unit tests |
| Crash recording / black box | guide (states table) | — | — (SDK-side, P2) | S4, S11, S12 |
| Application assembly (Init/Shutdown) | guide (states table) | Not-part-of-API note | `custom_binding_example` (today's form) | S7 |

## 6. Application assembly — today and after P0b

```cpp
// TODAY (no SDK): zero init — libraries instrument, classic logging just works.
int main() {
  xmotion::telemetry::SetResource("robot.id", "bot-01");
  RunRobot();                       // events -> spdlog; metrics aggregate; spans/signals parked
}

// AFTER P0b (application links xmTelemetry and owns the machinery):
#include "xmtelemetry/sdk.hpp"
int main() {
  namespace tel = xmotion::telemetry;
  tel::SdkConfig cfg;
  cfg.channel = tel::ChannelKind::kBlackBox;          // crash-survivable ring (P2)
  cfg.blackbox_path = "/dev/shm/bot01.ring";
  tel::SetResource("robot.id", "bot-01");
  tel::Init(std::move(cfg));                          // BEFORE any RT activity
  RunRobot();                                         // same call sites, now recorded/exported
  tel::Shutdown(std::chrono::seconds(2));             // bounded, accounted (D12)
}
```

A third form exists today for special cases (tests, bridges, bare-metal-ish targets): implement the `Binding` seam yourself — see `examples/custom_binding_example.cpp`, which captures every verb with ~100 lines and no dependencies.
