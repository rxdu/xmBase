# xmBase Telemetry — API Reference

Everything below lives in `namespace xmotion::telemetry` (abbreviated `tel::`) unless noted; include `xmbase/telemetry/telemetry.hpp` (the one header) for all of it. Each entry states **RT** (hot-path safety) and **Unbound** (behavior with no SDK bound; see [design.md §3](design.md) for the binding states — note the built-in console binding makes events behave like classic console logging by default).

Conventions that apply everywhere:

- **Names and format strings must be string literals** (static storage) — backends intern by pointer.
- Instrument names: lowercase dot-separated `<subsystem>.<name>`, unit suffix where applicable (`_us`, `_ms`, `_c`, `_hz`).
- All hot-path calls are `noexcept` and allocation-free.

---

## Time (`time.hpp`)

| Symbol | Description |
|---|---|
| `Clock`, `Timestamp`, `Duration` | Aliases of the family types (`xmotion::Clock` = `steady_clock`, ns resolution). The ONE time base — never introduce a parallel clock. |
| `Timestamp Now() noexcept` | Current monotonic time (vDSO-cheap). Contract: **one clock read per record** — stamp once, pass it along. RT: yes. |

## Context spine (`context.hpp`)

| Symbol | Description |
|---|---|
| `struct TraceId { uint64 hi, lo; }` / `struct SpanId { uint64 value; }` | W3C-shaped ids; `valid()` = nonzero; `operator==`. |
| `struct Context { TraceId trace; SpanId span; }` | The propagated pair. Zero ⇒ "none" (`valid()` false). |
| `Context NewTrace() noexcept` | Mint a fresh root (new 128-bit trace + root span). First call per thread seeds the id generator (may touch `std::random_device`) — mint at ingress/init, not deep inside a hard-RT loop. |
| `Context CurrentContext() noexcept` / `void SetCurrentContext(Context) noexcept` | The calling thread's current context (thread-local POD). Prefer `ContextGuard` to bare `Set` — a leaked context pollutes the next task on a pooled thread. |
| `class ContextGuard` | RAII set-and-restore: `tel::ContextGuard g(ctx);`. Non-copyable. RT: yes. |
| `Inject(Context) → std::array<uint8_t, kContextWireSize>` | Serialize for a message envelope (24 bytes, host byte order — intra-robot; W3C `traceparent` conversion is bridge-layer work). RT: yes. |
| `Extract(const uint8_t*, size_t) → Context` | Parse from an envelope. Short/null input ⇒ invalid Context (check `valid()` — boundary validation is yours). RT: yes. |

## Events / logging (`event.hpp`)

| Symbol | Description |
|---|---|
| `enum class Severity` | `kTrace(0) … kFatal(5)`, plus `kOff(6)` — **filter value only**, never an emit severity. |
| `class EventSource` / `GetEventSource(const char*) → EventSource` | Interned subsystem tag ("imu.front"). Register once at init; id 0 = anonymous. Value type. |
| `XM_TRACE/XM_DEBUG/XM_INFO/XM_WARN/XM_ERROR/XM_FATAL(fmt, ...)` | fmt-style event (`{}` placeholders, fmt spec syntax). Args: arithmetic, `string_view`, `const char*` — richer types are formatted by the caller OFF the hot path (compile error otherwise). Deferred format: args copied into a bounded stack pack (≤8 args, ≤160 B; strings truncate); formatting happens at the backend. RT: yes (when enabled; suppressed severities cost one gate check). Unbound: ≥ Warn → stderr via a minimal formatter; below Warn dropped. |
| `XM_*_SRC(src, fmt, ...)` | Source-attributed forms. |
| `XM_*_STREAM(expr << ...)` / `XM_*_STREAM_SRC(src, ...)` | Stream-style (ostream `<<`). Gated on the **runtime level before** the stringstream is built — a disabled stream log evaluates none of its arguments. Allocates when enabled ⇒ **non-RT convenience**. |
| `XM_EVENT(severity_expr, fmt, ...)` | Generic form taking a runtime `Severity` expression; not strippable by the compile floor — prefer the leveled macros. |
| `bool ShouldLog(Severity) noexcept` | The runtime gate (backend's level; unbound: `sev >= kWarn`). Use to skip expensive message construction. |
| `SetLogLevel(Severity) / GetLogLevel() noexcept` | Runtime minimum severity (was `XM_LOG_LEVEL`). Routed to the backend; unbound: set is a no-op, get reports `kWarn`. |
| `SetResource(string_view key, string_view value)` | Process/robot identity (OTel "resource"); set once at startup. Non-RT. Unbound: no-op. |
| `XM_TELEMETRY_LEVEL` (macro, define at build) | Compile-time floor: leveled macro call sites strictly below it compile to **nothing** (0=keep all … 6=strip all). Handle-based calls are not macro-strippable; they compile to no-op slot writes instead. |

## Metrics (`handles.hpp`)

Value-type handles over slots fixed at registration. **Register at init, use on the hot path**; handles are trivially copyable and safe to hold across SDK `Shutdown()` (slot memory is process-lifetime). Handles acquired while *unbound* point at shared no-op slots permanently — they do not upgrade when a backend binds later.

| Symbol | Hot-path op | Description |
|---|---|---|
| `Counter` ← `GetCounter(name)` | `Add(double v = 1.0)` | Monotonic accumulation (CAS add). |
| `Gauge` ← `GetGauge(name)` | `Set(double)` | Last-value (relaxed store). |
| `Histogram` ← `GetHistogram(name)` | `Record(double)` | count/sum/min/max **+ 24 exponential (power-of-two) buckets** (ABI v3) — percentile-capable; `Record` adds one `ilogb` + one relaxed increment. |
| `SignalChannel<T>` ← `GetChannel<T>(name)` | `Publish(const T&, Timestamp = Now())` | High-rate typed samples for the recording plane. `T` must be trivially copyable and `sizeof(T) <= kMaxSignalPayload` (192) — decompose larger data per-sample (a trajectory publishes points, not a blob). Unbound: no-op. |

All metric ops: RT yes, `noexcept`, no binding lookup per call. `Get*` may allocate (registration) — never call in a loop.

## Traces (`span.hpp`)

| Symbol | Description |
|---|---|
| `class Span` | RAII span: mints a child span of the current context, installs itself as current (nested scopes parent correctly; events inside carry its span id), emits ONE record at destruction `{name, ctx, parent, begin, end, links}`. Orphan scopes (no enclosing trace) self-root. Non-copyable. RT: yes (2× `Now()` + id + one seam call). Unbound: context bookkeeping still runs; nothing emitted. |
| `Span(const char* name)` / `Span(name, Context link)` | Plain / single-link constructors. |
| `void Span::AddLink(Context) noexcept` | Causally associate another context (OTel span **link** — association, not reparenting). Bounded by `kMaxSpanLinks` (4); overflow and invalid contexts are silently dropped (links are metadata, never control flow). Use for fan-in: one `AddLink` per consumed input. |
| `XM_SPAN(name)` | Block-scoped anonymous span. |
| `XM_SPAN_LINKED(name, ctx)` | Block-scoped span with one link — the "stage consumed exactly one upstream input" case. |

## Health (`health.hpp`)

| Symbol | Description |
|---|---|
| `enum class HealthState` | `kOk, kDegraded, kFault, kDisconnected`. |
| `ReportHealth(const char* subsystem, HealthState, const char* detail = "") noexcept` | Report state **transitions**, not steady state (apply hysteresis at the caller — don't flap). RT: yes. Unbound: non-Ok → stderr; Ok silent. Bound: routed as attributed records. |

## The seam — for SDK implementers (`binding.hpp`)

Users never touch these; they define what a backend must provide.

| Symbol | Description |
|---|---|
| `kBindingAbiVersion` (currently 3: histogram buckets; 2: span links) | Layout gate; `InstallBinding` rejects mismatches. |
| `struct Binding` | Function-pointer table: registration entries (`get_counter/gauge/histogram/signal`, `intern_source` — may allocate) and hot-path entries (`should_log`, `set_level/get_level`, `emit_event`, `emit_event_dyn`, `emit_span` (with `links, link_count`), `emit_signal`, `report_health` — must be `noexcept`, allocation-free, wait-free). `set_resource` non-RT. Pointer args (`name`, `fmt`, `links`) are valid only for the call unless documented literal. |
| `bool InstallBinding(const Binding*) noexcept` | Install/replace; `nullptr` unbinds. **Explicit calls are authoritative**: they disable auto-adoption of the console binding for the rest of the process. Contract for implementers: keep the outgoing table callable until the swap is visible; never free metric slots. |
| `const Binding* ActiveBinding() noexcept` | The active binding (may lazily adopt the console binding on first use — see design.md §3). |
| `detail::ArgPack` / `detail::CounterSlot/GaugeSlot/HistogramSlot/SignalSlot` | The deferred-format pack and slot layouts the handles write to. |
| `QosClass { kDiagnostics, kSignal }` | Drop-accounting classes (the SDK guarantees a signal flood can never evict a diagnostic). |

## Not part of this API (by design)

`Init()`, `Shutdown(deadline)`, `SdkConfig`, sinks, and exporters are **xmTelemetry SDK** surface (`xmtelemetry/sdk.hpp`) — the application's concern, never a library's. Libraries link xmBase and call the verbs; whoever owns `main()` decides the machinery.
