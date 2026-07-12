/*
 * message_slot.hpp
 *
 * MessageSlot<T, Storage, EventCountPolicy> — a single-slot message
 * container: writes overwrite, reads see the newest without consuming. The
 * wait-free depth-1 exchange ("latest value wins"); for the length-N case
 * (a recent window, newest-first snapshots) use MessageBuffer<T, N>
 * (message_buffer.hpp).
 *
 * Guarantees:
 *
 *   1. Store() never blocks and never fails for capacity reasons — the slot
 *      is overwritten (writer is WAIT-FREE: a fixed number of relaxed word
 *      stores plus two fences, no loops, no locks, regardless of readers).
 *   2. Load() returns the newest value or nothing, never a torn value: a
 *      seqlock validates the copy and retries on a concurrent overwrite.
 *      Writer-progress-only: readers retry, the writer NEVER waits — under
 *      a continuously-storing writer a reader is lock-free, not wait-free
 *      (each retry means the writer made progress). This is the documented
 *      bias (xmMessaging design.md POSIX-shm seqlock; R7 grants the writer
 *      wait-freedom).
 *   3. Overwrite accounting is the CALLER's job — the writer cannot count
 *      per-reader overwrites exactly without a lock, so callers that need
 *      it embed an ordinal in T and account one level up (that is what
 *      xmMessaging's per-subscriber ordinal-gap accounting does).
 *   4. T is the caller's whole record — callers that need stamps, ordinals,
 *      or headers embed them in T (xmMessaging stores a MailRecord<P> here).
 *
 * Facade over Core A (docs/concurrency.md "facades over shared cores"):
 * MessageSlot is literally a seqlock ring of ONE cell —
 * detail::SeqlockRing<T, 1, ...> (detail/seqlock_ring.hpp), which carries
 * the memory-ordering proofs (the W1/W2/R1/R2 fence pairs, promoted
 * verbatim from xmMessaging detail/message_buffer.hpp per ADR 0007 W1, plus
 * the W3/R0 cursor pair and the lap-detection argument added with the
 * depth-N generalization). This header freezes the depth-1 API; the depth-N
 * facade over the same core is MessageBuffer<T, N> (message_buffer.hpp).
 *
 * Concurrency contract: SINGLE writer at a time (two unserialized writers
 * would corrupt the sequence — callers with shared ownership serialize their
 * writers one level up). Any number of readers.
 *
 * T must be trivially copyable (word-wise copy). Non-trivially-copyable
 * values use MutexMessageSlot below — see its comment for the stated
 * divergence.
 *
 * Storage provides the cell storage (heap by default; a caller-provided
 * region — e.g. a shared mapping — via RegionStorage: same algorithm,
 * zero changes below); EventCountPolicy is carried for the parking verbs (never
 * touched by Store/Load) — see storage.hpp / event_count.hpp for the seam.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>

#include "xmbase/concurrency/detail/seqlock_ring.hpp"
#include "xmbase/concurrency/event_count.hpp"
#include "xmbase/concurrency/storage.hpp"

namespace xmotion {
namespace concurrency {

template <typename T, typename Storage = HeapStorage,
          typename EventCountPolicy = CondvarEventCount>
// See docs/concurrency.md for the family buffer taxonomy (which type to
// use when) and the facades-over-cores structure; depth-1 here is a contract
// choice, not a mechanism limit — the N-deep newest-first generalization
// over the same core is MessageBuffer<T, N>.
class MessageSlot {
 public:
  static_assert(std::is_trivially_copyable_v<T>,
                "MessageSlot requires a trivially copyable T (the seqlock "
                "copies words); non-trivially-copyable values use "
                "MutexMessageSlot");
  // The same cell type may be placed inside shared mappings (P1b), where the
  // atomics must be address-free — guaranteed iff always lock-free
  // ([atomics.lockfree]/4), which holds on both tested baselines (R1).
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "region-capable seqlock requires lock-free (address-free) "
                "64-bit atomics");

  // Bytes of placed storage one slot consumes — a region-layout computation
  // needs it (e.g. xmMessaging's posix_shm segment layout); every user
  // derives the same value from the same T, which is what makes offsets a
  // pure function of the placed type. Layout is the core's documented carve
  // (one cell, then the cursor header — detail/seqlock_ring.hpp).
  static constexpr std::size_t StorageBytes() noexcept {
    return Ring::StorageBytes();
  }

  // The storage instance decides WHERE the cell lives (heap vs a shared
  // region) and WHETHER this constructor initializes it. Heap storage
  // always initializes; a region ATTACHER must not (P1b: the cell may
  // already hold live data — the warm-start value that survives a writer
  // restart). The Store/Load algorithm is storage-blind.
  explicit MessageSlot(Storage storage = Storage()) : ring_(storage) {}

  MessageSlot(const MessageSlot&) = delete;
  MessageSlot& operator=(const MessageSlot&) = delete;

  // Writer side. Wait-free: bounded straight-line code, no loops, no locks.
  // Single writer at a time (see the concurrency contract above).
  void Store(const T& value) noexcept { ring_.Store(value); }

  // Reader side. Returns false if the slot was never written. Never blocks
  // the writer; retries while the writer is mid-store (writer progress).
  bool Load(T& out) const noexcept { return ring_.Load(out); }

  // Bounded read for the CROSS-PROCESS reach (P1b), where the writer can be
  // SIGKILLed mid-Store — see the core's LoadBounded comment (the M4 crash
  // story): a dead writer's permanently-odd sequence is DETECTED (kStalled)
  // instead of spun on forever.
  using LoadResult =
      typename detail::SeqlockRing<T, 1, Storage, EventCountPolicy>::LoadResult;

  LoadResult LoadBounded(T& out, std::uint32_t max_retries) const noexcept {
    return ring_.LoadBounded(out, max_retries);
  }

  // Crash repair (P1b) — wiring path of a (re)claiming WRITER only, while it
  // is provably the slot's sole writer. See the core's comment.
  void RepairAfterWriterCrash() noexcept { ring_.RepairAfterWriterCrash(); }

  // Parking seam (bounded parking verbs only; NEVER touched by Store/Load —
  // see event_count.hpp).
  EventCountPolicy& event_count() noexcept { return ring_.event_count(); }

 private:
  using Ring = detail::SeqlockRing<T, 1, Storage, EventCountPolicy>;

  Ring ring_;
};

// Fallback slot for movable-but-not-trivially-copyable values.
//
// STATED DIVERGENCE (P0b interim): this slot takes a small mutex on both
// sides, so the wait-free/allocation-free hot-path guarantee (R7) holds for
// trivially copyable values only — which is also the only class of value
// for which it CAN hold (copying a std::vector allocates no matter what the
// caller does). Depth-1 only — there is no mutex-based depth-N buffer (the
// MessageBuffer<T, N> contract requires trivially copyable T;
// docs/concurrency.md). Upgrade path if a consumer ever needs a lock-free
// rich-type slot: pointer-rotation triple buffer (P1 candidate).
template <typename T>
class MutexMessageSlot {
 public:
  MutexMessageSlot() = default;
  MutexMessageSlot(const MutexMessageSlot&) = delete;
  MutexMessageSlot& operator=(const MutexMessageSlot&) = delete;

  void Store(const T& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    value_ = value;
    written_ = true;
  }

  bool Load(T& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!written_) {
      return false;
    }
    out = value_;
    return true;
  }

 private:
  mutable std::mutex mutex_;
  T value_{};
  bool written_ = false;
};

}  // namespace concurrency
}  // namespace xmotion
