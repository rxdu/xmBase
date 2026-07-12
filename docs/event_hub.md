# EventHub — design draft (charter-reserved, build gated on first consumer)

- Status: **Designed, not built** (ADR 0007 charters/gates; this document is the committed replacement design for the event/ module removed at 0.5.0)
- Home: `xmbase/concurrency/event_hub.hpp` when built
- Tier: component-internal typed eventing — tier 1 of the three-tier eventing map in [concurrency.md](concurrency.md); never grows QoS/reaches (that is xmMessaging) or consumption/priority/GUI-marshaling (that is quickviz `core/event`)

## What it is for — and pointedly not for

EventHub carries **control-flow events inside one component**: stage-completion notifications, mode changes, fault signals, lifecycle transitions — occasional, must-not-be-lost, one-to-many. It is NOT a data path: high-rate samples belong in `MessageSlot`/`MessageBuffer`/`SpscQueue` (the buffers exist precisely so an event system never becomes a data plane). This division is what lets EventHub make the lossless promise cheaply.

## Contract

1. **Instance-based.** A component constructs its own `EventHub` and passes it where needed. No `GetInstance()`, ever — wiring is explicit (ADR 0005 doctrine; the removed module's singleton is the counterexample).
2. **Typed.** `Subscribe<E>(handler)` / `Emit(E{...})` — the event's C++ type IS the channel. Compile-time checked; no string keys, no `shared_ptr<BaseEvent>` downcasts. Two logical channels of the same payload shape = two distinct event structs (explicit, cheap, self-documenting).
3. **Lossless.** Every event reaches every subscriber registered at emit time. Sync dispatch is trivially lossless (runs on the emitter's thread); async delivery is lossless-or-refused: a full inbox makes `Emit` return `kWouldBlock` for that subscriber — the emitter decides (retry, escalate, treat as fault) and the refusal is counted. Silent loss is banned; unbounded growth is banned (R7); the tension between them is resolved the family way — an explicit status at the call site.
4. **Bounded.** Async inbox depth is declared per subscription at wiring time; memory is fixed from construction (R7). Sync dispatch uses no storage at all.
5. **No hidden threads (R3).** v1 is thread-lending only: async subscribers drain on a thread the component owns (`Drain()` / `DrainFor(bound)` parking on the eventcount). An owned-worker convenience mode is an explicitly-constructed later extension if a consumer demands it — never a default, never implicit.
6. **RAII subscriptions.** `Subscribe` returns a `Subscription` handle; scope exit unregisters. No unsubscribe ceremony, no dangling handlers.
7. **Not a hot path.** `Emit` takes a mutex over the subscriber snapshot (registration is wiring-time-rare; events are control-flow-rate). This is a deliberate, documented simplicity choice — if profiling ever shows Emit on someone's hot path, the correct fix is moving that traffic to a buffer, not making EventHub lock-free.

## Sketch (the wish-code, which is the spec)

```cpp
using xmotion::concurrency::EventHub;

// The component's internal events — types ARE the channels.
struct StageComplete { int stage; uint64_t t_us; };
struct PipelineFault { int stage; FaultCode code; };

class NavPipeline {  // a component's multi-threaded internals
  EventHub hub_;     // instance-owned; passed to stages by reference

  void WireStages() {
    // Sync subscriber: runs on the EMITTER's thread at Emit — for cheap,
    // non-blocking reactions (counters, latching a flag).
    fault_sub_ = hub_.Subscribe<PipelineFault>(
        [this](const PipelineFault& f) { fault_latch_.Set(f); });

    // Async subscriber: bounded inbox (depth declared), drained on the
    // coordinator's own thread — the thread-lending mode.
    done_sub_ = hub_.SubscribeAsync<StageComplete>(/*depth=*/16);
  }

  void StageWorker(int stage) {              // any worker thread
    // ... compute ...
    if (hub_.Emit(StageComplete{stage, NowUs()}) != EventHub::kOk) {
      // a subscriber's inbox was full: lossless-or-refused, MY decision.
      HandleBackpressureFault(stage);
    }
  }

  void CoordinatorLoop() {                   // component-owned thread
    while (running_) {
      done_sub_.DrainFor(10ms, [&](const StageComplete& e) {
        OnStageDone(e);                      // lossless, in order per emitter
      });
    }
  }

  EventHub::Subscription fault_sub_, done_sub_gone_;  // RAII: scope = registration
  EventHub::AsyncSubscription<StageComplete> done_sub_;
};
```

## Mapping from the removed module

| Removed (event/) | EventHub |
|---|---|
| `EventDispatcher::GetInstance()` + `RegisterHandler(name, fn)` | instance `hub_.Subscribe<E>(fn)` — the type replaces the name, the instance replaces the singleton |
| `Emit(source, "name", args...)` → `shared_ptr<BaseEvent>` | `hub_.Emit(E{...})` — typed, no allocation for small E (sync path copies on the stack; async path copies into the preallocated inbox) |
| `AsyncEventDispatcher` (hidden worker + unbounded ThreadSafeQueue) | `SubscribeAsync<E>(depth)` + component-owned drain — bounded, thread-lending, refusal counted |
| Handler priorities / consumption | deliberately absent — GUI-plane semantics, quickviz owns them |

## Internals (when built — all existing verified pieces)

Per async subscription: one `SpscQueue<E>` (emitters serialized by the Emit mutex satisfy the single-producer contract) + the subscription's `EventCount` for parking. Sync handlers: a mutex-guarded registration list, snapshotted at Emit. Nothing new to verify at the concurrency level — EventHub is composition of the taxonomy's built primitives, which is why it can wait for its consumer without design risk.

## The gate

Built when the first real consumer lands (candidates: nav multi-threaded processing; a GUI-style app on xmBase without quickviz). The build arrives with unit tests (registration/RAII churn, lossless-or-refused accounting, drain ordering), TSan/ASan/valgrind legs like everything in `concurrency/`, and a worked example. Open question deliberately left to that consumer: whether `DrainFor`'s handler-callback shape or a `TryTake(E&)` pull shape reads better at their call site — decide against real code, not taste.
