# xmbase/concurrency — the family buffer taxonomy

- Status: charter map per ADR 0007 (charters fix destination, gates fix timing)
- Rule of the page: every commonly-used inter-thread handoff structure has ONE named home here — built or reserved — so no component reinvents one. Reserved entries are contracts waiting on their first consumer, never speculative code.

## Which buffer do I want?

Pick by **read semantics first**, payload shape second:

1. *"I only ever want the newest value"* → `MessageBuffer<T>` (single-slot message container: writes overwrite, reads see the newest without consuming). Non-trivially-copyable payload → `MutexMessageBuffer<T>`; large payloads at high rate → TripleBuffer (reserved, below).
2. *"I must see every value, in order, and consume each once"* → `SpscQueue<T>` (bounded, reject-on-full is the default policy — overflow is the caller's explicit decision).
3. *"I want the recent history, newest-first, without consuming"* → depth-N MessageBuffer (reserved).
4. *"Several producers, one consumer"* → MpscQueue (reserved); until it exists: one SpscQueue per producer, consumer sweeps (the fan-in pattern xmMessaging uses).
5. *"Preallocated slots I loan out and return"* → FixedPool (reserved; xmMessaging's `LoanPool` is the incubating instance).

## Built (v0.5.0)

| Type | Contract (one line) | Verified by |
|---|---|---|
| `MessageBuffer<T, Storage, EventCountPolicy>` | depth-1, wait-free writer, torn-free non-consuming reads; overwritten-unread is correct behavior | seqlock tear-check under TSan; aarch64 CI; placement-equivalence test |
| `MutexMessageBuffer<T>` | same contract for non-trivially-copyable T (documented divergence: mutex, not wait-free) | unit tests |
| `SpscQueue<T, Storage>` | bounded FIFO, single producer + single consumer BY CONTRACT, TryPush fails when full (caller decides: reject/coalesce/shed), consuming TryPop | SPSC stress; TSan; aarch64 |
| `EventCount` / `CondvarEventCount` | lost-wakeup-free parking (eventcount protocol); futex form is shm-capable | lost-wakeup hammer; TSan |
| `HeapStorage` / `RegionStorage` | where cells live (heap vs caller-provided region, e.g. a shared mapping) | placement-equivalence test |

## Reserved (charter home fixed; built when the first consumer arrives)

| Type | Contract | Likely first consumer (the gate) |
|---|---|---|
| depth-N `MessageBuffer` (`Snapshot(out, k)`) | keep the last N; newest-first non-consuming snapshots; writer laps invalidate only the tail of a snapshot | quickviz `bridges/xmotion` scope widget (time-window plots) |
| `TripleBuffer<T>` | lock-free latest-value with STABLE READER BORROW for payloads too big to copy per read (camera frames, point clouds) | viz bridge / perception pipelines |
| `SpscQueue` drop-oldest overflow policy | lossy stream: overwrite the oldest instead of rejecting the newest (quickviz `RingBuffer`'s semantic, generalized; drops counted per the no-silent-loss doctrine) | first family consumer wanting lossy-FIFO (viz bridge chart feed is the candidate) |
| `MpscQueue<T>` | bounded multi-producer fan-in, single consumer | xmMessaging shared-ownership publishers (mutex-serialized today by declared design; P1 TODO) |
| `FixedPool<T>` | wiring-time-sized slot pool, allocation-free loan/return | promote xmMessaging's `LoanPool` when a second consumer appears |

Reserving an entry means: the name, the one-line contract, and the row in this table. No headers, no code, no tests until the consumer is real — then the implementation arrives WITH its verification (gate 2) like everything else here.

## Structure: facades over shared cores

User-facing types are **contract-named facades**; underneath, each is a thin wrapper over one of a deliberately small set of verified cores. The dividing line between cores is synchronization topology — that is what a memory-ordering proof attaches to, so one core never carries two proofs:

| Core | Topology | Facades it serves |
|---|---|---|
| **Seqlock overwrite ring** (writer-only progress; readers retry, never consume) | one writer cursor, N cells, per-cell sequence | `MessageBuffer<T>` (N=1, today's implementation), depth-N `MessageBuffer`/`HistoryBuffer` (reserved) |
| **SPSC cursor ring** (consumer writes state — the head; cells transfer ownership) | head/tail cursors | `SpscQueue<T>` reject-on-full (built), drop-oldest policy (reserved) |
| **Swap chain** (pointer rotation, stable reader borrow) | 2–3 slots, atomic swap | `TripleBuffer<T>` (reserved) |

Consequences: (1) adding a reserved facade means instantiating or parameterizing an existing core, not authoring a new concurrent algorithm; (2) facade APIs are frozen at first release, which is what makes internal core unification safe to do later without consumer churn (a `MessageBuffer` user cannot observe whether the inside is a bespoke slot or a ring of 1); (3) the mechanical extraction of Core A into an N-parameterized form happens in the same wave as the first N>1 consumer — not before (gates).

## Deliberate exclusions (do not add these here)

- **Byte-stream rings** (span writes, peek, partial consume, reset): transport-plumbing shape, driver-specific by the W1 fit audit — lives with xmDriver's `async_port`. It is not a record container and generalizing it here would blur both contracts.
- **Unbounded anything**: banned by R7. The deprecated `container/thread_safe_queue` is the cautionary example.
- **UI event dispatch** (consumption, priorities, GUI-thread marshaling): a different plane entirely (ADR 0007 §2) — quickviz `core/event` owns it.

## The quickviz correspondence

quickviz core stays family-dependency-free (ADR 0007 §4), so it keeps its own buffers. The concepts correspond; the code deliberately does not:

| quickviz (standalone) | family equivalent | note |
|---|---|---|
| `RingBuffer<T, N>` (drop-oldest, mutex) | `SpscQueue` + reserved drop-oldest policy | both bounded, both lossy-by-policy |
| `DoubleBuffer<T>` (write/swap, TryPull once per frame) | `MessageBuffer` (TC) / reserved `TripleBuffer` (borrow) | same latest-value role, different mechanics |
| `core/event/*` | — | different plane, no equivalent by design |

Keeping this table current is part of changing either side.
