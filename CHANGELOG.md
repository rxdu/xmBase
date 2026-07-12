# Changelog

Earlier releases (≤ v0.4.0) were recorded as GitHub release notes only; this file is the repo-side record from v0.5.0 on (the xmMessaging convention).

## v0.5.0 — 2026-07-12

The ADR 0007 W1 foundation wave: the verified concurrency primitives move in, the family testing harness is unified here, and the Eigen boundary becomes a formal target split.

### Added

- **`xmbase/concurrency/`** (namespace `xmotion::concurrency`) — the family's verified concurrency primitives, promoted from xmMessaging `detail/` with their full verification transferring:
  - `LatestSlot<T, Placement, Waiter>` — wait-free seqlock depth-1 exchange (Boehm construction; TSan-clean), with bounded crash-safe reads (`LoadBounded`) and sole-writer crash repair; `MutexLatestSlot<T>` fallback for non-trivially-copyable values.
  - `BoundedQueue<T, Placement>` — lock-free SPSC bounded ring, allocation-free after wiring, control block placeable in shared regions.
  - `FutexWaiter` / `CondvarWaiter` — eventcount parking policies (futex form is the TSan-verifiable baseline choice and carries the shared-word shape for cross-process use).
  - `HeapPlacement` / `RegionPlacement` — the placement seam: one algorithm implementation serves process-private heap cells and caller-provided shared regions (xmMessaging's shm segment supplies the region at W2).
  - Verification suite where the code now lives: seqlock tear check under concurrent load (checksummed payload, runs under TSan), SPSC conservation stress, futex lost-wakeup hammer, mutex-fallback equivalence, and Region-vs-Heap placement equivalence as a permanent test.
- **`xmbase/testing/`** (namespace `xmotion::testing`, header-only, test/bench tier ONLY — never linked by runtime code) — the family measurement toolkit, unified from its three prior generations (xmMessaging `bench/harness.hpp`, quickviz ingestion harness, xmTelemetry perf tier): nearest-rank tail percentiles, batched measurement loops with warmup, hardware-context capture, allocation probe (all operator new/delete forms replaced, nothrow included), and the machine-readable JSON report emitter (superset of the xmmessaging-bench-v1 schema).
- **`bench/xmbase_bench`** — xmBase pins its own primitive numbers: publish/take-shaped micro rows (latest store/load, queue push/pop, contended store) with the allocation gate armed; smoke run registered with ctest.
- **CI**: TSan lane now covers the concurrency primitive suite (with the documented `-Wno-tsan` for GCC 13's fence warning); new `ubuntu-24.04-arm` build+test job (weak-memory-model coverage); new `eigen-free-core` job proving the core builds with no Eigen installed.

### Changed

- **Target split (ADR 0007 Eigen boundary)**: `xmotion::xmBase` is now the Eigen-FREE core (types wire tier, telemetry, serialization, concurrency, testing, and the deprecated container/event tiers). The new `xmotion::xmBaseGeometry` target carries the Eigen tier (`types/geometry.hpp`, `types/quantities.hpp`, the `types/types.hpp` umbrella and `geometry_types.hpp` facade) and the `Eigen3::Eigen` usage requirement. Consumers of geometry types link `xmotion::xmBaseGeometry` in addition to the core (link map per ADR 0007: telemetry/messaging → core; driver/nav → core + geometry). `find_package(xmBase)` needs no Eigen for core-only consumers; geometry consumers may request `COMPONENTS geometry` for a clear configure-time diagnostic.
- `math/` removed — its single function repatriated to xmNavigation estimation (ADR 0007 promotion criteria applied to an existing resident; xmBase#27).

### Deprecated (removal at 0.6.0)

- **`xmbase/container/`** (`RingBuffer`, `ThreadSafeQueue`) — superseded by `xmbase/concurrency/` (`BoundedQueue` for SPSC record queues). The byte-stream `RingBuffer` shape (burst span I/O + peek) has one consumer, xmDriver's `async_port`, which repatriates the type at W3.
- **`xmbase/event/`** (event dispatcher/emitter pair) — retires together with `container/thread_safe_queue` (its only consumer is xmBase's own async dispatcher). Successors by plane: UI/command events belong to the viz library's GUI event system; observability/robot data belongs to xmMessaging topics (ADR 0007 §2).
