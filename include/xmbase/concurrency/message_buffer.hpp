/*
 * message_buffer.hpp
 *
 * MessageBuffer<T, N, Storage, EventCountPolicy> — the depth-N message
 * buffer. Taxonomy contract (docs/concurrency.md): keep the last N;
 * newest-first non-consuming snapshots; writer laps invalidate only the
 * tail of a snapshot.
 *
 * Facade over Core A (the seqlock overwrite ring,
 * detail/seqlock_ring.hpp — one writer cursor, N cells, per-cell sequence),
 * the same core MessageSlot wraps at N = 1. The core carries the
 * memory-ordering proofs (W1/W2/R1/R2 fence pairs, W3/R0 cursor pair) and
 * the lap-detection argument that backs the Snapshot guarantee: every
 * returned snapshot is torn-free, newest-first, with strictly consecutive
 * descending write positions — a concurrent writer can only shorten the
 * tail.
 *
 * Guarantees, in MessageSlot's terms (same writer, same reader bias):
 *
 *   1. Store() is WAIT-FREE and never fails for capacity reasons — the
 *      oldest of the N values is overwritten.
 *   2. LoadLatest() returns the newest value or nothing, never torn;
 *      readers retry only while the writer makes progress (lock-free
 *      readers, wait-free writer).
 *   3. Snapshot(out, k) captures up to min(k, N, values stored) of the most
 *      recent values, newest-first, into CALLER-PROVIDED storage — no
 *      allocation, single pass, wait-free; the count returned is what
 *      survived concurrent overwrites (possibly 0 under a racing writer).
 *      The out-pointer + count signature follows the module's convention
 *      (out-parameter reads, wiring-time-sized caller storage — R7); C++17
 *      has no std::span, and an internal buffer would impose a copy or a
 *      lifetime contract the seqlock cannot give.
 *   4. Reads NEVER consume: every reader sees the same history; overwrite
 *      accounting stays the caller's job (embed an ordinal in T).
 *
 * NAMING NOTE — not a FreeRTOS "Message Buffer": FreeRTOS's primitive of
 * that name is a variable-length CONSUMING byte FIFO (a stream of framed
 * bytes, each read dequeues). This type is the opposite on every count: an
 * N-slot OVERWRITE RING of fixed-size records with NON-CONSUMING
 * newest-first snapshots — every reader can take the same snapshot, nothing
 * is ever dequeued, and the writer overwrites the oldest instead of
 * blocking. If you need FIFO + consuming semantics, use SpscQueue. Only the
 * newest value needed? MessageSlot (message_slot.hpp) is the depth-1 form.
 * Non-trivially-copyable values have no depth-N home: MutexMessageSlot is
 * depth-1 only (docs/concurrency.md).
 *
 * Concurrency contract: SINGLE writer at a time, any number of readers
 * (the core's contract).
 *
 * Storage/EventCountPolicy: the same seams as MessageSlot (storage.hpp /
 * event_count.hpp); the region carve layout is documented in the core
 * header — N cells then the cursor header, offsets a pure function of T
 * and N (xmMessaging's shm layout consumes this at W2).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "xmbase/concurrency/detail/seqlock_ring.hpp"
#include "xmbase/concurrency/event_count.hpp"
#include "xmbase/concurrency/storage.hpp"

namespace xmotion {
namespace concurrency {

template <typename T, std::size_t N, typename Storage = HeapStorage,
          typename EventCountPolicy = CondvarEventCount>
class MessageBuffer {
 public:
  static_assert(N >= 1, "MessageBuffer needs a depth of at least 1");
  static_assert(std::is_trivially_copyable_v<T>,
                "MessageBuffer requires a trivially copyable T (the seqlock "
                "ring copies words); non-trivially-copyable values have no "
                "depth-N home — MutexMessageSlot is depth-1 only (see the "
                "taxonomy, docs/concurrency.md)");

  // The compile-time depth (also available as capacity() for generic code).
  static constexpr std::size_t kDepth = N;

  // Region-layout constants (see the carve layout in the core header):
  // total placed bytes, and the per-cell stride within the cell array.
  static constexpr std::size_t StorageBytes() noexcept {
    return Ring::StorageBytes();
  }
  static constexpr std::size_t CellBytes() noexcept {
    return Ring::CellBytes();
  }

  // Same storage semantics as MessageSlot: heap always initializes; a
  // region attacher preserves the live history it finds (P1b warm start).
  explicit MessageBuffer(Storage storage = Storage()) : ring_(storage) {}

  MessageBuffer(const MessageBuffer&) = delete;
  MessageBuffer& operator=(const MessageBuffer&) = delete;

  // Writer side — wait-free, overwrites the oldest of the N values.
  void Store(const T& value) noexcept { ring_.Store(value); }

  // Latest value (identical to MessageSlot::Load).
  bool LoadLatest(T& out) const noexcept { return ring_.Load(out); }

  // Bounded latest read for the cross-process reach (P1b) — the depth-N
  // analogue of MessageSlot::LoadBounded, same LoadResult contract.
  using LoadResult =
      typename detail::SeqlockRing<T, N, Storage,
                                   EventCountPolicy>::LoadResult;

  LoadResult LoadLatestBounded(T& out,
                               std::uint32_t max_retries) const noexcept {
    return ring_.LoadBounded(out, max_retries);
  }

  // Newest-first non-consuming snapshot of up to k values into out[0..k);
  // out must hold at least min(k, kDepth) values. Returns the count
  // captured. ORDERING GUARANTEE (backed by the core's lap-detection
  // argument, detail/seqlock_ring.hpp — same standing as the fence pairs):
  //
  //   - out[0] is the newest published value at the moment the writer
  //     cursor was captured; every returned entry i was written at exactly
  //     position (cursor-1) - i — STRICTLY CONSECUTIVE DESCENDING write
  //     positions, no gaps, no reordering, ever.
  //   - every returned entry is torn-free (each cell copy is seqlock-
  //     validated AND index-matched before it counts).
  //   - a concurrent writer can only SHORTEN THE TAIL of what is returned
  //     (laps overwrite oldest-first, so the first failed cell proves every
  //     older cell is gone too); it can never corrupt an entry, reorder
  //     entries, or create a gap. The count may be less than
  //     min(k, kDepth, values stored) — down to 0 under a racing writer —
  //     and what IS returned always satisfies the three points above.
  std::size_t Snapshot(T* out, std::size_t k) const noexcept {
    return ring_.Snapshot(out, k);
  }

  static constexpr std::size_t capacity() noexcept { return N; }

  // Crash repair — wiring path of a (re)claiming sole writer (see the core
  // comment): retracts the in-flight cell only; the published history stays
  // readable.
  void RepairAfterWriterCrash() noexcept { ring_.RepairAfterWriterCrash(); }

  // Parking seam (bounded parking verbs only; never touched by
  // Store/Load/Snapshot).
  EventCountPolicy& event_count() noexcept { return ring_.event_count(); }

 private:
  using Ring = detail::SeqlockRing<T, N, Storage, EventCountPolicy>;

  Ring ring_;
};

}  // namespace concurrency
}  // namespace xmotion
