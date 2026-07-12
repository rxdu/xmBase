# xmbase/concurrency — the family buffer taxonomy

- Status: charter map per ADR 0007 (charters fix destination, gates fix timing)
- Rule of the page: every commonly-used inter-thread handoff structure has ONE named home here — built or reserved — so no component reinvents one. Reserved entries are contracts waiting on their first consumer, never speculative code.

## Which buffer do I want?

Pick by **read semantics first**, payload shape second:

1. *"I only ever want the newest value"* → `MessageSlot<T>` (single-slot message container: writes overwrite, reads see the newest without consuming). Non-trivially-copyable payload → `MutexMessageSlot<T>`; large payloads at high rate → TripleBuffer (reserved, below).
2. *"I must see every value, in order, and consume each once"* → `SpscQueue<T>` (bounded, reject-on-full is the default policy — overflow is the caller's explicit decision).
3. *"I want the recent window, newest-first, without consuming"* → `MessageBuffer<T, N>` (built — the depth-N message buffer; snapshots are non-consuming and a writer lap only shortens a snapshot's tail).
4. *"Several producers, one consumer"* → MpscQueue (reserved); until it exists: one SpscQueue per producer, consumer sweeps (the fan-in pattern xmMessaging uses).
5. *"Preallocated slots I loan out and return"* → FixedPool (reserved; xmMessaging's `LoanPool` is the incubating instance).

## Built (v0.5.0)

| Type | Contract (one line) | Verified by |
|---|---|---|
| `MessageSlot<T, Storage, EventCountPolicy>` | depth-1, wait-free writer, torn-free non-consuming reads; overwritten-unread is correct behavior | seqlock tear-check under TSan; aarch64 CI; placement-equivalence test |
| `MessageBuffer<T, N, Storage, EventCountPolicy>` | keep the last N; newest-first non-consuming snapshots (strictly consecutive descending write positions, torn-free); writer laps invalidate only the tail of a snapshot | lap stress under max-rate writer (checksummed payload, tail-shortening observed, TSan); depth-1 equivalence vs `MessageSlot`; crash-poison (in-flight cell only) + repair; placement equivalence |
| `MutexMessageSlot<T>` | same depth-1 contract for non-trivially-copyable T (documented divergence: mutex, not wait-free; depth-1 only) | unit tests |
| `SpscQueue<T, Storage>` | bounded FIFO, single producer + single consumer BY CONTRACT, TryPush fails when full (caller decides: reject/coalesce/shed), consuming TryPop | SPSC stress; TSan; aarch64 |
| `EventCount` / `CondvarEventCount` | lost-wakeup-free parking (eventcount protocol); futex form is shm-capable | lost-wakeup hammer; TSan |
| `HeapStorage` / `RegionStorage` | where cells live (heap vs caller-provided region, e.g. a shared mapping) | placement-equivalence test |

## Reserved (charter home fixed; built when the first consumer arrives)

| Type | Contract | Likely first consumer (the gate) |
|---|---|---|
| `TripleBuffer<T>` | lock-free latest-value with STABLE READER BORROW for payloads too big to copy per read (camera frames, point clouds) | viz bridge / perception pipelines |
| `SpscQueue` drop-oldest overflow policy | lossy stream: overwrite the oldest instead of rejecting the newest (quickviz `RingBuffer`'s semantic, generalized; drops counted per the no-silent-loss doctrine) | first family consumer wanting lossy-FIFO (viz bridge chart feed is the candidate) |
| `MpscQueue<T>` | bounded multi-producer fan-in, single consumer | xmMessaging shared-ownership publishers (mutex-serialized today by declared design; P1 TODO) |
| `FixedPool<T>` | wiring-time-sized slot pool, allocation-free loan/return | promote xmMessaging's `LoanPool` when a second consumer appears |
| `EventHub` | component-internal typed eventing — **fully designed** ([event_hub.md](event_hub.md): instance-based, type-as-channel, lossless-or-refused with counted refusals, bounded inboxes, thread-lending drain, RAII subscriptions; composition of the built primitives, nothing new to verify) | nav multi-threaded processing, or a GUI-style app on xmBase without quickviz |

Reserving an entry means: the name, the one-line contract, and the row in this table. No headers, no code, no tests until the consumer is real — then the implementation arrives WITH its verification (gate 2) like everything else here.

Gate-override record: the depth-N `MessageBuffer` row moved from Reserved to Built on 2026-07-12 by owner override of its first-consumer gate (the "complete shape" decision — the seqlock family ships as slot + ring + queue in one wave), ahead of the quickviz `bridges/xmotion` scope widget it was gated on. The verification arrived with it, per gate 2.

## Structure: facades over shared cores

User-facing types are **contract-named facades**; underneath, each is a thin wrapper over one of a deliberately small set of verified cores. The dividing line between cores is synchronization topology — that is what a memory-ordering proof attaches to, so one core never carries two proofs:

| Core | Topology | Facades it serves |
|---|---|---|
| **Seqlock overwrite ring** (writer-only progress; readers retry, never consume) | one writer cursor, N cells, per-cell sequence — `detail::SeqlockRing<T, N>` (N-parameterized, built) | `MessageSlot<T>` (LITERALLY a ring of 1), `MessageBuffer<T, N>` (built) |
| **SPSC cursor ring** (consumer writes state — the head; cells transfer ownership) | head/tail cursors | `SpscQueue<T>` reject-on-full (built), drop-oldest policy (reserved) |
| **Swap chain** (pointer rotation, stable reader borrow) | 2–3 slots, atomic swap | `TripleBuffer<T>` (reserved) |

Consequences: (1) adding a reserved facade means instantiating or parameterizing an existing core, not authoring a new concurrent algorithm; (2) facade APIs are frozen at first release, which is what makes internal core unification safe to do later without consumer churn (a `MessageSlot` user cannot observe whether the inside is a bespoke slot or a ring of 1); (3) the extraction of Core A into its N-parameterized form (`detail/seqlock_ring.hpp`) happened with the depth-N wave (gate owner-overridden 2026-07-12, the "complete shape" decision — see the record under Reserved): `MessageSlot` is now literally a ring of 1 over the shared core, and the depth-1 suite passing unmodified across the swap is the facade-freeze claim exercised for real.

## The three eventing tiers (which mechanism do I want?)

The axis is the COMPONENT BOUNDARY, not the thread/process boundary:

| Tier | Home | Loss contract | Who may use it |
|---|---|---|---|
| Inside one component | this module (`EventHub`, reserved; primitives, built) | lossless delivery to registered handlers | everyone — components and applications |
| Between composed components (any reach: in-process, IPC, inter-host) | xmMessaging | QoS-governed; drop-tolerance is a feature | applications only (components never link messaging — ADR 0005) |
| GUI applications | quickviz `core/event` | lossless + consumption + priority + GUI-thread marshaling | quickviz apps |

If two pieces of code are parts of one component: this module. If they are components being composed: messaging — even when both live in one process (messaging's in-process reach exists so a one-process deployment can split later without rewiring, M6). `EventHub` will never grow QoS, cross-boundary topics, or reaches — needing those means you have outgrown it and want messaging. Messaging will never grow handler callbacks (R3).

## Coverage of the deprecated modules (both removed at 0.5.0; ring_buffer relocated to xmDriver the same wave — xmDriver#33)

Every capability of the deprecated pair, and where it went. Family-wide audit (2026-07-12): zero consumers outside xmBase itself for event/ and thread_safe_queue; ring_buffer's only consumer is xmDriver's async_port facade — so removal carries zero migration.

**container/ring_buffer:** record FIFO -> `SpscQueue` (built); lossy variant -> drop-oldest policy (reserved); span writes/reads, Peek/PeekAt, partial consume, Reset -> repatriated WITH the type to xmDriver (landed: xmDriver#33) — these are protocol-parser semantics (byte-stream scanning, frame resync), not inter-thread handoff, and a peekable partially-consumable "queue" has no clean concurrency contract; multi-producer tolerance -> driver's copy keeps its mutex; generic MP fan-in is the reserved `MpscQueue`.

**container/thread_safe_queue:** FIFO handoff -> `SpscQueue`; blocking Pop -> `EventCount::WaitFor(bound, predicate)` + `TryPop` (bounded and shutdown-aware — see the worker-inbox example); multi-producer -> reserved `MpscQueue` (until then: one SpscQueue per producer, consumer sweeps); multi-CONSUMER work-stealing -> deliberately out of scope (never used in the family; a new gated row if it ever appears); unboundedness -> deliberately dropped (R7) — a feature removal, not a gap.

**event/ (removed at 0.5.0 — superseded pending redesign, not abandoned):** the CAPABILITY — component-internal typed eventing for multi-threaded code — is charter-reserved as `EventHub` above; it was placed in xmBase intentionally (components cannot link messaging, so foundation tier is the only home that serves component internals). What retires at 0.6.0 is the pre-doctrine IMPLEMENTATION: the `GetInstance()` global singleton (wiring must be explicit and instance-owned, ADR 0005), string-keyed `shared_ptr<BaseEvent>` runtime downcasts (typed subscriptions instead), and unbounded ThreadSafeQueue delivery with a hidden worker (bounded SpscQueue + EventCount; thread ownership explicit). Async-dispatch-as-a-pattern -> the worker-inbox example today, `EventHub`'s async mode when built. Push-style callbacks ("call me when X" on an arbitrary thread) are deliberately not offered anywhere in the family — poll with bounded parks is the doctrine (R3).

## Deliberate exclusions (do not add these here)

- **Byte-stream rings** (span writes, peek, partial consume, reset): transport-plumbing shape, driver-specific by the W1 fit audit — lives with xmDriver's `async_port`. It is not a record container and generalizing it here would blur both contracts.
- **Unbounded anything**: banned by R7. The deprecated `container/thread_safe_queue` is the cautionary example.
- **UI event dispatch** (consumption, priorities, GUI-thread marshaling): a different plane entirely (ADR 0007 §2) — quickviz `core/event` owns it.
- **Multi-consumer work-stealing queues**: out of scope until a real consumer appears (see coverage map).

## The quickviz correspondence

quickviz core stays family-dependency-free (ADR 0007 §4), so it keeps its own buffers. The concepts correspond; the code deliberately does not:

| quickviz (standalone) | family equivalent | note |
|---|---|---|
| `RingBuffer<T, N>` (drop-oldest, mutex) | `SpscQueue` + reserved drop-oldest policy | both bounded, both lossy-by-policy |
| `DoubleBuffer<T>` (write/swap, TryPull once per frame) | `MessageSlot` (TC) / reserved `TripleBuffer` (borrow) | same latest-value role, different mechanics |
| `core/event/*` | — | different plane, no equivalent by design |

Keeping this table current is part of changing either side.
