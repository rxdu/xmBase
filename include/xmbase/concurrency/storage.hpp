/*
 * storage.hpp
 *
 * Storage policy: where a concurrency primitive's cells live. The slot and
 * ring algorithms (detail/seqlock_ring.hpp behind message_slot.hpp /
 * message_buffer.hpp, and spsc_queue.hpp) never say `new` — they
 * ask their Storage for storage, sized once at wiring time; nothing grows
 * afterwards.
 *
 * Promoted verbatim from xmMessaging detail/storage.hpp (renamed) (ADR 0007, W1).
 * Requirement/decision IDs in comments (R7, P0b, P1b, M2/M4) are xmMessaging's,
 * retained so the rationale keeps its provenance.
 *
 * Policy contract — what the seqlock ring (MessageSlot / MessageBuffer) and
 * SpscQueue require of a Storage P:
 *
 *   - `template <typename U> using SingleHandle` / `ArrayHandle`: handle
 *     types for one placed U / a placed U[]; must be dereferenceable like
 *     U* (owning or non-owning is the policy's business).
 *   - `p.MakeSingle<U>()` / `p.MakeArray<U>(count)`: acquire storage. Called
 *     at wiring (construction) time ONLY — never on a hot path.
 *   - `p.Initialize()`: whether the constructing algorithm may (and must)
 *     zero/value-initialize the cells it placed. `false` means the cells
 *     already hold live data that must be preserved (see RegionStorage).
 *
 * The two policies:
 *
 *   - HeapStorage: process-private heap cells, owning handles, always
 *     freshly zero-initialized (the P0b in-process reach).
 *   - RegionStorage: cells are carved out of a pre-sized caller-provided
 *     region (raw base pointer + size — this header knows NOTHING about how
 *     the region was mapped; a shared-memory owner such as xmMessaging's
 *     shm_segment supplies the mapping); handles are NON-OWNING raw pointers
 *     (the region owns the memory), and zero-initialization happens only
 *     when `initialize` is true — the region CREATOR (or a claimant
 *     re-claiming a slot region) initializes; every other attacher must
 *     preserve the contents it finds, because those contents are live data
 *     (the M2/M4 warm-start value survives a publisher restart precisely
 *     because attaching does not re-zero).
 *
 * This is the seam that lets a process-private user and a shared-mapping
 * user share ONE implementation of each algorithm: the algorithms consume a
 * storage INSTANCE (stateless for heap, a region cursor for regions) so
 * the same constructor body serves both; the seqlock/ring code itself never
 * changes — that was the parameterization bet, and P1b is where it paid out.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

namespace xmotion {
namespace concurrency {

// Heap storage: cells are process-private heap memory, allocated at
// wiring time only (never on a hot path — R7).
struct HeapStorage {
  template <typename U>
  using SingleHandle = std::unique_ptr<U>;

  template <typename U>
  using ArrayHandle = std::unique_ptr<U[]>;

  // Fresh heap memory: the algorithm zero-initializes (Initialize() below).
  template <typename U>
  SingleHandle<U> MakeSingle() {
    return std::make_unique<U>();
  }

  template <typename U>
  ArrayHandle<U> MakeArray(std::size_t count) {
    return std::make_unique<U[]>(count);
  }

  // Heap cells are always new — always initialize.
  static constexpr bool Initialize() noexcept { return true; }
};

// Region storage: a bump allocator over a fixed caller-provided region
// (typically a shared mapping, but this policy is mapping-agnostic — it sees
// only base + size). Handles are non-owning raw pointers; the region
// outlives them. Determinism contract: every user that attaches a region
// constructs the SAME algorithm objects in the SAME order over the same
// region, so the bump cursor yields identical offsets everywhere — the
// layout is a function of the placed types and the region constants, never
// of who attached first.
struct RegionStorage {
  template <typename U>
  using SingleHandle = U*;

  template <typename U>
  using ArrayHandle = U*;

  RegionStorage(unsigned char* base, std::size_t size,
                  bool initialize) noexcept
      : base_(base), size_(size), initialize_(initialize) {}

  template <typename U>
  SingleHandle<U> MakeSingle() {
    return Carve<U>(1);
  }

  template <typename U>
  ArrayHandle<U> MakeArray(std::size_t count) {
    return Carve<U>(count);
  }

  bool Initialize() const noexcept { return initialize_; }

  // Bytes consumed so far — used by a layout pass to size regions.
  std::size_t used() const noexcept { return cursor_; }

 private:
  template <typename U>
  U* Carve(std::size_t count) {
    static_assert(std::is_trivially_destructible_v<U>,
                  "region-placed cells are never destroyed (the region is "
                  "the owner); they must be trivially destructible");
    const std::size_t align = alignof(U) < 64 ? 64 : alignof(U);
    cursor_ = (cursor_ + align - 1) & ~(align - 1);
    unsigned char* at = base_ + cursor_;
    cursor_ += sizeof(U) * count;
    assert(cursor_ <= size_ &&
           "xmbase/concurrency: storage region overrun — layout constants "
           "disagree");
    if (initialize_) {
      // Value-initialize in place — the same semantics HeapStorage gets
      // from make_unique's `new U()` (zeroing atomics and PODs), so the
      // algorithms observe identical starting state on both placements.
      for (std::size_t i = 0; i < count; ++i) {
        ::new (static_cast<void*>(at + i * sizeof(U))) U();
      }
    }
    // When attaching (initialize_ == false) the cells were constructed by
    // the initializing user; launder the pointer for strictness.
    return std::launder(reinterpret_cast<U*>(at));
  }

  unsigned char* base_;
  std::size_t size_;
  std::size_t cursor_ = 0;
  bool initialize_;
};

}  // namespace concurrency
}  // namespace xmotion
