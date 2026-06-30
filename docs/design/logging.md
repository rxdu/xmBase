# xmSigma Logging — Design

Status: accepted (2026-07). Supersedes the original synchronous spdlog wrapper.

The logging infrastructure is used in high-performance robotics code, including
low-latency and real-time control loops. This document records the review of the
original design, the resulting contracts, and the dual-mode (soft-RT / hard-RT)
architecture.

## 1. What was wrong with the original design

The original `DefaultLogger` (spdlog wrapper) did the right things in the wrong
thread, at the wrong time:

- **Synchronous logger + flush-on-every-message.** `SetLoggerLevel` called
  `logger_->flush_on(active_level)`, so every message that passed the filter
  forced a sink flush — a blocking `write()`/flush syscall *inline in the caller*.
  In a 1 kHz loop that is one syscall per cycle: unbounded, jittery latency.
- **Always-on `stdout_color_sink_mt`.** Colored console I/O is slow and blocks if
  stdout is a pipe with a slow reader; the `_mt` mutex serializes all logging.
- **`GetInstance()` returned `shared_ptr` by value** → two atomic refcount ops per
  log call (even for level-filtered messages).
- **`_STREAM` macros formatted unconditionally** — a `std::stringstream` (locale +
  heap) was built and `.str()` allocated *before* any level check, so a disabled
  `XLOG_DEBUG_STREAM` in a hot loop still allocated every cycle.
- **Init race.** The singleton object was created by the magic-static, but
  `Initialize()` was gated separately by an atomic flag; a second thread could get
  the instance and call `Log()` before the first thread finished assigning
  `logger_` → null-deref / data race.
- **User data passed as the fmt format string** (`logger_->info(data)`) — `{`/`}`
  in the payload → `fmt::format_error` → dropped message + stderr noise.
- **`data.pop_back()` with zero args** → UB on an empty string.
- **Unchecked `item_data_[item_id]`** in CtrlLogger → OOB on a bad id.
- Macros were `{…}` blocks (not `do{…}while(0)`) → broke `if/else` without braces.

## 2. Contracts

Two paths, one backend. Both feed the **same** spdlog sinks (unified output).

| | Soft-RT (default) | Hard-RT (opt-in) |
|---|---|---|
| API | `XLOG_*` (unchanged) | `XLOG_RT_*` on an `RtLogger` |
| Where formatting happens | caller (fmt) | caller (`format_to_n` into fixed buffer) |
| Allocation in hot path | spdlog may allocate the queued message | **none** (fixed inline buffer) |
| Locking in hot path | queue mutex (brief) | **none** (lock-free SPSC) |
| Syscalls in hot path | none (worker does I/O) | none (vDSO clock only) |
| Overflow policy | configurable (block / overrun-oldest) | **drop + `dropped()` counter** (never blocks) |
| Worst case | best-effort, soft deadline | **bounded** (format time + memcpy + release store) |
| Use it when | ~everything | a loop with an explicit hard deadline |

**Soft-RT** = `spdlog::async_logger`: caller formats and enqueues, a worker thread
does the I/O. Flushing is periodic (`spdlog::flush_every`) and on shutdown — never
per message.

**Hard-RT** = per-`RtLogger` lock-free SPSC ring of fixed-size slots. The producer:
read a monotonic timestamp (vDSO), `format_to_n` into the slot's `char[N]`
(bounded, truncating, no heap), publish via a release store of the write index.
No locks, no syscalls, no allocation, never blocks. On a full ring it bumps
`dropped_` and returns. A dedicated drain thread pops records and forwards them to
the shared spdlog sinks for actual I/O.

### Why these choices
- **Drop-on-full, not block:** a hard-RT producer must never wait on the consumer
  (priority inversion). A bounded ring + visible drop count is the RT-correct
  policy. The soft path can afford to block/overwrite.
- **Format in the caller (into a fixed buffer), not deferred:** keeps the producer
  bounded + allocation-free while keeping the API ergonomic. Full *zero*-format
  (serialize raw args, format in the consumer — NanoLog/Quill style) is faster but
  constrains arg types to trivially-copyable and forces compile-time format
  strings; it is a documented escalation for a specific loop that profiles hot,
  not the default.
- **One backend:** the drain thread feeds the same sinks as the async logger, so
  RT and non-RT output interleave correctly in one file/console.

## 3. API sketch

```cpp
// Soft-RT (default) — signatures unchanged; async behind the scenes.
XLOG_INFO("v = {}", v);                 // fmt {}-style (NOT printf %d)
XLOG_DEBUG_STREAM("pose " << x << y);   // level-gated before formatting

// Compile-time floor: levels below XMSIGMA_ACTIVE_LEVEL vanish at compile time.
//   -DXMSIGMA_ACTIVE_LEVEL=2   // INFO; trace/debug compiled out

// Hard-RT (opt-in) — explicit logger owned by the RT component.
xmotion::RtLogger rt("ctrl_loop");      // owns ring + drain thread
for (;;) {                              // 1 kHz control loop
  XLOG_RT_INFO(rt, "tau = {}", tau);    // wait-free, no alloc, no syscall
}
// rt.dropped() -> records lost to ring-full (should be 0 in steady state)
```

## 4. Compile-out

`ENABLE_LOGGING=OFF` compiles all `XLOG_*` / `XLOG_RT_*` to no-ops.
`XMSIGMA_ACTIVE_LEVEL=<n>` additionally drops, at compile time, every site below
level `n` — so trace/debug cost exactly zero in a release RT build regardless of
the runtime level.

## 5. Flushing the data loggers

`CsvLogger` / `CtrlLogger` / `EventLogger` are async (spdlog `async_factory`).
Buffered rows are NOT guaranteed to reach disk on their own — call `Flush()`
before the program exits. Relying on `spdlog::shutdown()` alone is not reliable
for every logger (a singleton data logger that outlives the shutdown call can be
left undrained). `Flush()` posts a sink flush so the worker writes out what's
queued.

## 6. Testing & validation

- Unit tests pin each fix: brace-laden payloads logged verbatim (format-string
  injection), zero-arg `LogData()`/`LogEvent()` (the old `pop_back` UB), the
  `_STREAM` macros not evaluating their args when the level is disabled, macro
  use in an unbraced `if/else` (do-while hygiene), `CtrlLogger` header/data
  round-trip and the out-of-range-id guard.
- `RtLogger`: content-completeness (every record read back intact when not
  overflowing), drop-on-full, and oversized-message truncation.
- The suite runs clean under **ASan + UBSan**. Under **TSan**, `RtLogger` (the
  lock-free ring) is race-free; the async `DefaultLogger` path raises a
  "double lock" warning that reproduces with *bare* spdlog 1.9.2 (no xmSigma
  code) and does not deadlock — it is an upstream spdlog artifact, not ours.

## 7. Non-goals / future
- MPSC RT logger (multiple producers per ring) — current `RtLogger` is SPSC (one
  RT thread per logger, the common control-loop shape).
- Zero-format binary logging (see §2) — escalation, not default.
- Signal-safe logging — spdlog is not async-signal-safe; do not log from a signal
  handler.
