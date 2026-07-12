/*
 * latest_slot.hpp
 *
 * LatestSlot<T, Placement, Waiter> — a wait-free depth-1 exchange ("latest
 * value wins"), guarantees:
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
 * Promoted verbatim from xmMessaging detail/latest_slot.hpp (ADR 0007, W1);
 * requirement/decision IDs in comments (R1, R7, D15, P0b, P1b, M4) are
 * xmMessaging's, retained so the proofs keep their provenance.
 *
 * Data-race freedom (and TSan cleanliness) comes from the Boehm seqlock
 * construction: the record bytes live in an array of relaxed
 * std::atomic<uint64_t> words, ordered by the sequence counter and fences.
 * Memory-ordering pairs, referenced from the code below:
 *
 *   (W1) writer: seq.store(odd, relaxed) THEN atomic_thread_fence(release)
 *        THEN relaxed word stores — any reader whose relaxed word LOAD
 *        reads one of those stores has the writer's release fence
 *        synchronize with the reader's acquire fence (R2), so the reader's
 *        SECOND seq load observes the odd value (or later) and the copy is
 *        rejected. A torn read can never validate.
 *   (W2) writer: final seq.store(even, release) — pairs with the reader's
 *        FIRST seq.load(acquire) (R1): a reader that observes seq == s+2
 *        observes every word store that happened-before it.
 *   (R1) reader: first seq.load(acquire) — see (W2).
 *   (R2) reader: relaxed word loads THEN atomic_thread_fence(acquire) THEN
 *        second seq.load(relaxed) — see (W1).
 *
 * Concurrency contract: SINGLE writer at a time (two unserialized writers
 * would corrupt the sequence — callers with shared ownership serialize their
 * writers one level up). Any number of readers.
 *
 * T must be trivially copyable (word-wise copy). Non-trivially-copyable
 * values use MutexLatestSlot below — see its comment for the stated
 * divergence.
 *
 * Placement provides the cell storage (heap by default; a caller-provided
 * region — e.g. a shared mapping — via RegionPlacement: same algorithm,
 * zero changes below); Waiter is carried for the parking verbs (never
 * touched by Store/Load) — see placement.hpp / waiter.hpp for the seam.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <type_traits>

#include "xmbase/concurrency/placement.hpp"
#include "xmbase/concurrency/waiter.hpp"

namespace xmotion {
namespace concurrency {

// Polite spin hint for seqlock read retries (it only spins while the writer
// is making progress).
inline void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  asm volatile("yield" ::: "memory");
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);  // compiler barrier
#endif
}

template <typename T, typename Placement = HeapPlacement,
          typename Waiter = CondvarWaiter>
class LatestSlot {
 public:
  static_assert(std::is_trivially_copyable_v<T>,
                "LatestSlot requires a trivially copyable T (the seqlock "
                "copies words); non-trivially-copyable values use "
                "MutexLatestSlot");
  // The same cell type may be placed inside shared mappings (P1b), where the
  // atomics must be address-free — guaranteed iff always lock-free
  // ([atomics.lockfree]/4), which holds on both tested baselines (R1).
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "region-capable seqlock requires lock-free (address-free) "
                "64-bit atomics");

  // Bytes of placed storage one slot consumes — a region-layout computation
  // needs it (e.g. xmMessaging's posix_shm segment layout); every user
  // derives the same value from the same T, which is what makes offsets a
  // pure function of the placed type.
  static constexpr std::size_t StorageBytes() noexcept { return sizeof(Cell); }

  // The placement instance decides WHERE the cell lives (heap vs a shared
  // region) and WHETHER this constructor initializes it. Heap placement
  // always initializes; a region ATTACHER must not (P1b: the cell may
  // already hold live data — the warm-start value that survives a writer
  // restart). The Store/Load algorithm below is placement-blind.
  explicit LatestSlot(Placement placement = Placement())
      : cell_(placement.template MakeSingle<Cell>()) {
    if (placement.Initialize()) {
      // Explicit zero-init: an even, zero sequence means "never written".
      cell_->seq.store(0, std::memory_order_relaxed);
      for (std::size_t i = 0; i < kWords; ++i) {
        cell_->words[i].store(0, std::memory_order_relaxed);
      }
    }
  }

  LatestSlot(const LatestSlot&) = delete;
  LatestSlot& operator=(const LatestSlot&) = delete;

  // Writer side. Wait-free: bounded straight-line code, no loops, no locks.
  // Single writer at a time (see the concurrency contract above).
  void Store(const T& value) noexcept {
    Cell& c = *cell_;
    // Stage the value as words on the stack; the zero fill gives any
    // trailing padding a defined value so the stored words are reproducible.
    std::uint64_t staged[kWords] = {};
    std::memcpy(staged, &value, sizeof(T));

    const std::uint64_t s = c.seq.load(std::memory_order_relaxed);
    c.seq.store(s + 1, std::memory_order_relaxed);         // odd: in progress
    std::atomic_thread_fence(std::memory_order_release);   // (W1)
    for (std::size_t i = 0; i < kWords; ++i) {
      c.words[i].store(staged[i], std::memory_order_relaxed);
    }
    c.seq.store(s + 2, std::memory_order_release);         // (W2)
  }

  // Reader side. Returns false if the slot was never written. Never blocks
  // the writer; retries while the writer is mid-store (writer progress).
  bool Load(T& out) const noexcept {
    const Cell& c = *cell_;
    std::uint64_t staged[kWords];
    for (;;) {
      const std::uint64_t s1 = c.seq.load(std::memory_order_acquire);  // (R1)
      if (s1 == 0) {
        return false;  // never written
      }
      if ((s1 & 1u) != 0u) {  // write in progress — retry
        CpuRelax();
        continue;
      }
      for (std::size_t i = 0; i < kWords; ++i) {
        staged[i] = c.words[i].load(std::memory_order_relaxed);
      }
      std::atomic_thread_fence(std::memory_order_acquire);             // (R2)
      if (c.seq.load(std::memory_order_relaxed) == s1) {
        break;  // copy validated: no overwrite raced it
      }
      CpuRelax();
    }
    std::memcpy(&out, staged, sizeof(T));
    return true;
  }

  // Bounded read for the CROSS-PROCESS reach (P1b), where the writer can be
  // SIGKILLed mid-Store (impossible in-process: threads die with their
  // process). Same algorithm and ordering pairs as Load(); the only
  // difference is a retry budget so a dead writer's permanently-odd
  // sequence is DETECTED (kStalled) instead of spun on forever — the
  // "skippable sequence" half of the M4 crash story. kStalled can also
  // fire under pathological live-writer pressure (max_retries consecutive
  // overwrites during one read); the caller's fallback (the last value it
  // already took) is correct in both cases, because a value that never
  // finished its Store was never published.
  enum class LoadResult : std::uint8_t { kValue, kEmpty, kStalled };

  LoadResult LoadBounded(T& out, std::uint32_t max_retries) const noexcept {
    const Cell& c = *cell_;
    std::uint64_t staged[kWords];
    for (std::uint32_t attempt = 0; attempt <= max_retries; ++attempt) {
      const std::uint64_t s1 = c.seq.load(std::memory_order_acquire);  // (R1)
      if (s1 == 0) {
        return LoadResult::kEmpty;  // never written (or crash-repaired)
      }
      if ((s1 & 1u) != 0u) {  // write in progress — or writer died mid-store
        CpuRelax();
        continue;
      }
      for (std::size_t i = 0; i < kWords; ++i) {
        staged[i] = c.words[i].load(std::memory_order_relaxed);
      }
      std::atomic_thread_fence(std::memory_order_acquire);             // (R2)
      if (c.seq.load(std::memory_order_relaxed) == s1) {
        std::memcpy(&out, staged, sizeof(T));
        return LoadResult::kValue;
      }
      CpuRelax();
    }
    return LoadResult::kStalled;
  }

  // Crash repair (P1b) — wiring path of a (re)claiming WRITER only, while it
  // is provably the slot's sole writer (in xmMessaging: the
  // publisher-liveness claim in shm_segment.hpp). A writer SIGKILLed
  // mid-Store left seq odd and the words half-written; the in-flight value
  // was never published, so the honest repair is "never written": readers
  // fall back to their own last taken value (staleness keeps rising —
  // M4-A1), and the next Store starts a fresh even/odd cycle.
  void RepairAfterWriterCrash() noexcept {
    Cell& c = *cell_;
    if ((c.seq.load(std::memory_order_relaxed) & 1u) != 0u) {
      c.seq.store(0, std::memory_order_release);
    }
  }

  // Parking seam (bounded parking verbs only; NEVER touched by Store/Load —
  // see waiter.hpp).
  Waiter& waiter() noexcept { return waiter_; }

 private:
  static constexpr std::size_t kWords =
      (sizeof(T) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);

  struct Cell {
    std::atomic<std::uint64_t> seq;
    std::atomic<std::uint64_t> words[kWords];
  };

  typename Placement::template SingleHandle<Cell> cell_;
  Waiter waiter_;
};

// Fallback slot for movable-but-not-trivially-copyable values.
//
// STATED DIVERGENCE (P0b interim): this slot takes a small mutex on both
// sides, so the wait-free/allocation-free hot-path guarantee (R7) holds for
// trivially copyable values only — which is also the only class of value
// for which it CAN hold (copying a std::vector allocates no matter what the
// caller does). Upgrade path if a consumer ever needs a lock-free rich-type
// slot: pointer-rotation triple buffer (P1 candidate).
template <typename T>
class MutexLatestSlot {
 public:
  MutexLatestSlot() = default;
  MutexLatestSlot(const MutexLatestSlot&) = delete;
  MutexLatestSlot& operator=(const MutexLatestSlot&) = delete;

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
