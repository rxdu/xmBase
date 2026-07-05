# XMotion Telemetry — Application Coverage & OpenTelemetry Comparison

Two questions this document answers for anyone evaluating the API: *does it cover the instrumentation needs of real robotics applications?* and *how does it relate to the OpenTelemetry API it deliberately resembles?*

Companions: [blueprint.md](blueprint.md) (the whole module) · [guide.md](guide.md) (how to instrument) · [reference.md](reference.md) (contracts). Rationale: [ADR 0004](https://github.com/rxdu/xmotion/blob/main/docs/adr/0004-telemetry-layering.md) and the [research report](https://github.com/rxdu/xmotion/tree/main/docs/research) whose findings drove every divergence below.

## 1. Coverage of common realtime/robotics application patterns

Each row names the pattern, what the API provides, and where the claim is *proven* — a scenario in the executable spec and/or a runnable example in this repo.

| Application pattern | What the API provides | Proven by |
|---|---|---|
| **Hard/soft-RT control loops** (1 kHz servo/motor control) | Pre-registered value handles → relaxed atomics on the hot path; `XM_SCOPE` at two clock reads + one id; deferred-format events (arguments copied, formatting deferred); never-block / drop-and-count semantics; `XM_TELEMETRY_LEVEL` compile floor for stripped builds | S1 (alloc-free sweep, page-fault ban, one-clock-read rule), S5 (flood isolation); `instrumented_control_loop_example` |
| **Device drivers / sensor management** | `EventSource` per device (filterable, attributed faults); freshness-age gauge + `ReportHealth` **transition** convention with caller-side hysteresis — a 1:1 fit for xmDriver's `FreshnessMonitor`/`DeviceHealth` | S3; `device_health_example` |
| **Perception/planning pipelines** | One trace per iteration (`NewTrace`), a span per stage, cross-thread/process identity in 24-byte envelopes (`Inject`/`Extract`), **span links** for fan-in stages; stalls visible as span duration + latency histogram | S2; `trace_pipeline_example` |
| **Flight recording / incident forensics** | The `signal()` verb — a raw high-rate sample plane OTel does not have: fixed-size POD samples at loop rate, per-sample decomposition of variable-length data (delta D5), shaped for the crash-survivable mmap black box + MCAP | S1/S4/S11/S12 (recording plane lands P2–P3; the API is already shaped for it); `custom_binding_example` |
| **Multi-process robots** (control process + planner + ROS nodes) | ROS-free identity carriage in any envelope; one monotonic clock; per-host Collector topology; boot-id + wall-clock re-anchoring spec'd for RTC-less boots | S2-A1, S10 |
| **Weeks-long autonomous operation** | Registry caps with counted overflow (D13); process-lifetime metric slots — handles safe across shutdown and static destruction (the top real-world logging crash class); fork contract; counter wrap-safety | S7/S8/S9 — scenarios derived from the production-failure research sweep |
| **Fleet monitoring / teleoperation** | metrics + health → the OTLP path; `SetResource` process/robot identity | export lands at P3; API complete now |

**Honest gaps — deliberate, and recorded rather than accidental:**

- **No free-form key-value attributes** on spans/events/measurements. Bounded-cardinality discipline; `EventSource` is the lighter per-subsystem answer (delta D8); richer attribute support is an open question in the scenario catalog.
- **No sampling API.** Robotics *inverts* sampling (research report §7): record everything locally, filter at export — sampling is an exporter policy, not a call-site concern.
- **Async-executor context propagation is manual** (`ContextGuard` discipline, tested by S2-A5) — the general state of C++; there is no framework-blessed async context to hook.

## 2. Comparison with the OpenTelemetry API

The design rule (ADR 0004): **keep OTel's data model, drop its call path.** Everything that exports is OTel-shaped so `OtelSink` translation is mechanical and lossless; every hot-path mechanism that the research found unusable on a control loop was replaced with a registration-time-resolved, allocation-free equivalent.

| OpenTelemetry API | XMotion telemetry | Relationship |
|---|---|---|
| API / SDK / exporter layering; API no-ops without SDK | Identical layering; unbound = no-op **except events ≥ Warn and non-Ok health → stderr** | Adopted. The deviation is deliberate: a robot must never silently swallow faults (ADR 0004 §2). |
| `TracerProvider` / `MeterProvider` — per-call provider lookup | **Install-once binding** (ABI-gated fn table) + handles resolved at registration | Diverged — the core RT redesign. Provider lookup (locks, `shared_ptr` refcounts) is not 1 kHz-safe. |
| `Span`: heap `shared_ptr`, `SetAttribute`, `AddEvent`, `SetStatus`, explicit `End()` | `Scope`: stack RAII, end-reported single record; events inside inherit its identity automatically; no attributes/status (bounded records) | Diverged for RT. Span *events* are covered by ordinary `XM_*` calls inside the scope. |
| Span links | `AddLink` / `XM_SCOPE_LINKED`, bounded at `kMaxSpanLinks` | Parity (bounded). |
| W3C trace-context (`traceparent` text header) | Same 128/64-bit id shape; **binary** 24-byte envelope intra-robot; text conversion at the app/ROS bridge | Parity at the model level; wire format optimized for robot IPC. |
| `Counter` / `Histogram` / `Gauge` (+ `UpDownCounter`, observable/async instruments) | Same core trio as value handles over atomic slots | Adopted minus extras: observables are *inverted* (the drain samples slots — the callback never intrudes into RT code); `UpDownCounter` ≈ Gauge/Counter for our uses. |
| **Attributes on every measurement** (`Add(1, {{"motor","left"}})`) — dynamic series creation | Pre-registered full instrument names (`motor.left.faults`) | **Biggest divergence.** Per-measurement attribute sets are the canonical cardinality-explosion source (OTel added a 2000-series circuit breaker for it); our series set is bounded by construction + registry caps (D13). |
| Logs API (`Logger::EmitLogRecord`, eager attribute/body construction) | `XM_*` deferred-format macros: bounded stack `ArgPack`, formatting at the drain | Diverged — NanoLog-style capture is the RT-logging differentiator. Severity maps losslessly onto OTel's severity numbers. |
| `Context` / `Token` / `Attach` | Thread-local `Context` + `ContextGuard` | Same idea, simpler; pool-thread pollution is tested (S2-A5) rather than framework-managed. |
| Baggage | Absent | Deliberate: per-hop weight and unbounded cardinality; nothing in the family needs it. |
| Sampling API (`Sampler`, head-based) | Absent | Deliberate inversion (see gaps above). |
| Resource | `SetResource` | Parity. |
| Semantic conventions | Family naming convention (`<subsystem>.<name>` + unit suffix); OTel semconv mapping lives in `OtelSink` | Adopted at the sink, not the call site. |
| — | **`signal()`** high-rate recording verb; **health** convention; **compile-out floor**; **ABI-gated seam** with process-lifetime slots (static-destruction-safe) | XMotion-only — the robotics additions OTel has no counterpart for. |

### Interoperability guarantee

Because ids, severities, span semantics, and resource identity are OTel-shaped, the P3 `OtelSink` emits standard OTLP: traces land in Tempo/Jaeger, metrics in Prometheus/Grafana, logs in Loki — and a ROS bridge can inject/extract W3C `traceparent` without loss. Divergences live entirely on the *capture* side, where OTel's mechanisms were the problem being solved.

### Where OpenTelemetry is genuinely richer

Arbitrary attributes, baggage, span status/kind, observable instruments, ecosystem auto-instrumentation, and semconv breadth. Each is either (a) an intentional robotics inversion, (b) an SDK/exporter concern rather than an API one, or (c) a recorded open question in the [scenario catalog](https://github.com/rxdu/xmTelemetry/blob/main/docs/scenarios.md) — the same delta process (D1–D15) that shaped everything else decides if and when they land.
