# ADR 0001 — Foundation utilities in xmSigma (containers, math, serialization)

- Status: Accepted (RingBuffer + serialization tranches landed; further tranches pending)
- Date: 2026-06-30
- Scope: what generic, reusable data structures and algorithms belong in the Σ foundation

## Context

xmMu and xmNabla have begun re-implementing the same generic, dependency-light utilities in multiple places, and the copies are silently diverging. The trigger: `RingBuffer` exists as **two** copies — `mu/src/async_port/.../ring_buffer.hpp` and `nabla/.../quickviz/.../imview/.../ring_buffer.hpp` — that have drifted apart, with the μ copy carrying real bug fixes (release-mode OOB guard, single-lock atomic `Peek`, overwrite-accounting fix) that the ∇ copy lacks. Divergent copies of a generic container is exactly the failure the foundation layer exists to prevent.

A survey of `mu/src` and `nabla/src` for generic, reusable code (data structures + algorithms that are universal across the family, dependency-light, and spoken by more than one layer) found a cluster of such candidates.

## Decision

Establish a home in Σ for generic, **dependency-light** (std-only; no Eigen/asio/ROS) utilities, organized by kind:

- `xmsigma/container/` — generic data structures (this ADR adds `ring_buffer.hpp`).
- `xmsigma/math/` — angle/units/scalar math helpers (future tranche).
- `xmsigma/serialization/` — byte pack/unpack + checksums (future tranche).
- `xmsigma/util/` — generic patterns (FSM, event dispatcher, thread-safe queue) (future tranche).

The bar for promotion (Σ's charter): **universal across the family, spoken by more than one layer, and dependency-light.** Layer-specific code stays in its layer (driver protocol → μ; planning/control/estimation → ∇).

### Promoted now

- **`xmsigma/container/ring_buffer.hpp`** — the general-purpose, mutex-guarded circular buffer (the hardened μ version is the canonical copy). Explicitly distinct from the logging module's specialized **lock-free** SPSC/MPSC rings (`logging/rt_logger*.hpp`): use this for general buffering, those for the wait-free RT logging hot path. Documented in the header so there is no "which ring?" ambiguity.

### Evaluation — further candidates (ranked; from the survey)

| Candidate | Evidence | Deps | Verdict |
|-----------|----------|------|---------|
| **RingBuffer** | duplicated μ (`async_port`) + ∇ (`imview`), **drifted** | std-only | **PROMOTE now** + delete copies on re-pin |
| **ThreadSafeQueue** | 2 copies in ∇ (`common/event`, `imview`) | std-only | consolidate → promote (`util/`) |
| **Event dispatcher/emitter** | full module duplicated in ∇ (`common/event` vs `imview`) | std-only | consolidate → promote (`util/`) |
| **Byte pack/unpack (endian get/set)** | hand-rolled in vesc, sbus, waveshare drivers (4+ sites) | std-only | **PROMOTED** — `serialization/byte_order.hpp` |
| **Checksum/CRC family** (CRC-8 Maxim, CRC-16, additive) | re-derived per driver (ddsm, SCServo, vesc) | std-only | **PROMOTED** — `serialization/checksum.hpp` |
| **FSM template** (`std::variant`-based) | single generic copy in ∇ `control/fsm` | std-only | promote (`util/`); fix stale include guard |
| **Angle/units math** (`WrapToPi`, deg/rad, rpm↔rad/s) | hand-rolled in ∇ kinematics; μ `units.hpp` speaks the same | `<cmath>` | promote (`math/`) |
| **clamp/saturate/lerp** | bespoke `Clamp` in vesc + sim motor | std-only | prefer `std::clamp`; bundle `saturate`/`lerp` into `math/` if wanted |

### Deliberately kept in their layer

- `hal::Result<T>` — generic shape but coupled to `hal::Status`; keep in μ unless a generic `Result<T,E>` is explicitly wanted.
- `math_utils/matrix.hpp` (∇) — pulls Eigen; violates Σ's dependency-light charter.
- Planning graph priority queues, imview UI buffers — single-layer, domain-specific.

## Consequences & migration

- New `container/` area in Σ; `ring_buffer.hpp` ships header-only via the existing `install(DIRECTORY include/)`. No new build deps.
- **Consolidation (follow-up, per copy):** re-pin μ (and ∇) to the new Σ, replace each in-tree `RingBuffer` with a facade/`using` alias (or include swap) and **delete the divergent copy in the same change**, so the bug fixes are locked in and drift cannot recur. Verified by each component's CI and the umbrella Assembly job.
- Further tranches (queue/event → `util/`, byte+CRC → `serialization/`, angle math → `math/`) are separate ADR-tracked PRs, each promoting the consolidated version and deleting duplicates.

## Status

Landed:
- `xmsigma/container/ring_buffer.hpp` + `test/test_ring_buffer.cpp` (overwrite-accounting across laps, PeekAt range, Peek/Read clamping). μ consolidated onto it (its drifted copy deleted); ∇'s quickviz copy kept by design.
- `xmsigma/serialization/byte_order.hpp` (endian load/store, signed variants) and `xmsigma/serialization/checksum.hpp` (CRC-8/MAXIM-DOW, CRC-16/XMODEM, inverted-sum), matched to the family's devices and pinned to standard catalogue check values in `test/test_serialization.cpp`.

Promotions are additive/non-breaking; the μ-driver consolidation onto the serialization helpers (VESC, DDSM, SCServo, SBUS) is the follow-up, replacing each hand-rolled routine with the Σ primitive after re-pin. Remaining tranches: `util/` (ThreadSafeQueue, event dispatcher, FSM template) and `math/` (angle/units).
