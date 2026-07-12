/*
 * detail/seqlock_ring.hpp
 *
 * detail::SeqlockRing<T, N, Storage, EventCountPolicy> — Core A of the
 * family buffer taxonomy (docs/concurrency.md, "facades over shared cores"):
 * the seqlock overwrite ring. One writer cursor, N cells, per-cell sequence.
 *
 * NOT a user-facing type: user code holds one of the contract-named facades
 * over this core — MessageSlot<T> (a ring of 1, message_slot.hpp) or
 * MessageBuffer<T, N> (depth-N newest-first snapshots, message_buffer.hpp).
 * The facades freeze the APIs; this core carries the memory-ordering proofs.
 *
 * Cell layout: each of the N cells carries its own sequence word (the
 * odd/even seqlock protocol below), the WRITE INDEX of the value it holds
 * (the monotone 64-bit position the writer stored there — the lap-detection
 * mechanism), and the T payload as relaxed atomic words. The single writer
 * owns one cursor, `write_count`: Store() writes cell[write_count % N]
 * under that cell's seqlock, then publishes write_count + 1.
 *
 * Promoted verbatim from xmMessaging detail/message_buffer.hpp via
 * xmbase message_buffer.hpp (ADR 0007, W1); requirement/decision IDs in
 * comments (R1, R7, D15, P0b, P1b, M4) are xmMessaging's, retained so the
 * proofs keep their provenance.
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
 * Ring generalization — the cursor pair and the lap-detection argument
 * (new at depth-N, same rigor as the pairs above):
 *
 *   (W3) writer: write_count.store(idx + 1, release) AFTER (W2) — pairs
 *        with the reader's write_count.load(acquire) (R0): a reader that
 *        observes write_count == c observes the completed cell publish
 *        (the (W2) even-sequence store) of every position < c.
 *   (R0) reader: write_count.load(acquire) — see (W3). The captured value
 *        c names the newest published position, c-1.
 *
 *   The write index is stored between the (W1) fence and the (W2) release
 *   EXACTLY like a payload word, so a validated copy is a consistent
 *   (index, payload) pair: the same argument that forbids a torn payload
 *   forbids a payload paired with another store's index.
 *
 *   Lap detection: position p lives in cell p % N, and the values
 *   successively written to that cell carry indexes p, p+N, p+2N, ...
 *   (single writer, monotone cursor). A reader holding a captured cursor c
 *   that VALIDATES cell (c-1-i) % N and finds index != c-1-i has therefore
 *   proven the writer lapped (wrote position >= c) since c was captured —
 *   every other value that cell can validly hold is newer by a multiple of
 *   N. Consequences:
 *
 *     - Read-latest (Load/LoadBounded): index != c-1 means the captured c
 *       is stale — reload c and retry. Same reader bias as the seqlock
 *       retry itself: writer-progress-only (every retry means the writer
 *       published at least N more values), so readers stay lock-free under
 *       a continuously-storing writer and the writer stays WAIT-FREE.
 *
 *     - Snapshot(out, k): walk i = 0 .. min(k, c, N)-1 over cells
 *       (c-1-i) % N, newest-first, requiring each validated index to equal
 *       exactly c-1-i. Every walked position is already PUBLISHED (i < c),
 *       so within one walk a validation failure — odd sequence, sequence
 *       changed across the copy, or index mismatch — can only mean a lap
 *       reached that cell. The writer overwrites cells in position order
 *       (position c first, which is cell (c-N) % N — the walk's OLDEST
 *       entry), so by the time a lap reaches walk entry i (overwrite
 *       position c-1-i+N), every older entry j > i (overwrite position
 *       c-1-j+N < c-1-i+N) has already been overwritten. Stopping at the
 *       first failure therefore loses nothing recoverable, and the entries
 *       already taken (0..i-1) validated with exact indexes: a returned
 *       snapshot is torn-free, newest-first, with strictly consecutive
 *       descending indexes — a concurrent writer can only SHORTEN THE
 *       TAIL. The walk never retries, so Snapshot is wait-free (bounded
 *       straight-line work); the price is that a racing lap may shorten it
 *       (down to 0 entries in the N=1 degenerate case — read-latest is the
 *       verb that guarantees a value).
 *
 * Concurrency contract: SINGLE writer at a time (two unserialized writers
 * would corrupt the sequence — callers with shared ownership serialize their
 * writers one level up). Any number of readers.
 *
 * T must be trivially copyable (word-wise copy). Non-trivially-copyable
 * values use MutexMessageSlot (message_slot.hpp) — depth-1 only; see
 * its comment for the stated divergence.
 *
 * Storage provides the cell storage (heap by default; a caller-provided
 * region — e.g. a shared mapping — via RegionStorage: same algorithm,
 * zero changes below); EventCountPolicy is carried for the parking verbs
 * (never touched by Store/Load/Snapshot) — see storage.hpp /
 * event_count.hpp for the seam.
 *
 * Region carve layout (deterministic — a pure function of T and N, so every
 * attacher of a shared region derives identical offsets; xmMessaging's shm
 * segment layout consumes this at W2). The ring performs exactly TWO carves,
 * in this order:
 *
 *   1. MakeArray<Cell>(N)     — at the region cursor rounded up to 64;
 *                               cell stride is CellBytes(); cell 0's FIRST
 *                               member is its sequence word (the region's
 *                               first word is pointer-interconvertible with
 *                               it — crash tooling and tests rely on this).
 *   2. MakeSingle<Header>()   — the write_count cursor, at the next
 *                               64-aligned offset (RoundUp(N*CellBytes(),
 *                               64) from the ring's base).
 *
 * StorageBytes() is exactly the bytes those two carves consume from a
 * 64-aligned start.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "xmbase/concurrency/event_count.hpp"
#include "xmbase/concurrency/storage.hpp"

namespace xmotion {
namespace concurrency {

// Polite spin hint for seqlock read retries (it only spins while the writer
// is making progress). Lives with the core; exported at namespace level
// (not detail) because it predates the core split as public API.
inline void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  asm volatile("yield" ::: "memory");
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);  // compiler barrier
#endif
}

namespace detail {

template <typename T, std::size_t N, typename Storage = HeapStorage,
          typename EventCountPolicy = CondvarEventCount>
class SeqlockRing {
 public:
  static_assert(N >= 1, "SeqlockRing needs at least one cell");
  static_assert(std::is_trivially_copyable_v<T>,
                "the seqlock ring requires a trivially copyable T (the "
                "seqlock copies words); non-trivially-copyable values use "
                "MutexMessageSlot (depth-1 only — docs/concurrency.md)");
  // The same cell type may be placed inside shared mappings (P1b), where the
  // atomics must be address-free — guaranteed iff always lock-free
  // ([atomics.lockfree]/4), which holds on both tested baselines (R1).
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "region-capable seqlock requires lock-free (address-free) "
                "64-bit atomics");

  static constexpr std::size_t kDepth = N;

  // Read outcome of the bounded verbs — see LoadBounded below.
  enum class LoadResult : std::uint8_t { kValue, kEmpty, kStalled };

  // Bytes of placed storage the ring consumes — a region-layout computation
  // needs it (e.g. xmMessaging's posix_shm segment layout); every user
  // derives the same value from the same T and N, which is what makes
  // offsets a pure function of the placed types. See the carve layout in
  // the header comment: N cells (64-aligned base) then the cursor header
  // (64-aligned).
  static constexpr std::size_t StorageBytes() noexcept {
    return RoundUpCacheLine(sizeof(Cell) * N) + sizeof(Header);
  }

  // Per-cell stride inside the carved cell array (cell i's sequence word is
  // at byte offset i * CellBytes() from the ring's base carve).
  static constexpr std::size_t CellBytes() noexcept { return sizeof(Cell); }

  // The storage instance decides WHERE the cells live (heap vs a shared
  // region) and WHETHER this constructor initializes them. Heap storage
  // always initializes; a region ATTACHER must not (P1b: the cells may
  // already hold live data — the warm-start values that survive a writer
  // restart). The Store/Load/Snapshot algorithms below are storage-blind.
  explicit SeqlockRing(Storage storage = Storage())
      : cells_(storage.template MakeArray<Cell>(N)),
        header_(storage.template MakeSingle<Header>()) {
    if (storage.Initialize()) {
      // Explicit zero-init: an even, zero sequence means "never written";
      // a zero cursor means "nothing published".
      header_->write_count.store(0, std::memory_order_relaxed);
      for (std::size_t n = 0; n < N; ++n) {
        cells_[n].seq.store(0, std::memory_order_relaxed);
        cells_[n].index.store(0, std::memory_order_relaxed);
        for (std::size_t i = 0; i < kWords; ++i) {
          cells_[n].words[i].store(0, std::memory_order_relaxed);
        }
      }
    }
  }

  SeqlockRing(const SeqlockRing&) = delete;
  SeqlockRing& operator=(const SeqlockRing&) = delete;

  // Writer side. Wait-free: bounded straight-line code, no loops (beyond
  // the fixed word copy), no locks. Single writer at a time (see the
  // concurrency contract above); the writer OWNS the cursor, so its load
  // is relaxed.
  void Store(const T& value) noexcept {
    const std::uint64_t idx =
        header_->write_count.load(std::memory_order_relaxed);
    Cell& c = cells_[idx % N];
    // Stage the value as words on the stack; the zero fill gives any
    // trailing padding a defined value so the stored words are reproducible.
    std::uint64_t staged[kWords] = {};
    std::memcpy(staged, &value, sizeof(T));

    const std::uint64_t s = c.seq.load(std::memory_order_relaxed);
    c.seq.store(s + 1, std::memory_order_relaxed);         // odd: in progress
    std::atomic_thread_fence(std::memory_order_release);   // (W1)
    // The write index is fence-protected exactly like a payload word.
    c.index.store(idx, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kWords; ++i) {
      c.words[i].store(staged[i], std::memory_order_relaxed);
    }
    c.seq.store(s + 2, std::memory_order_release);         // (W2)
    header_->write_count.store(idx + 1, std::memory_order_release);   // (W3)
  }

  // Reader side, latest value. Returns false if nothing was ever published
  // (or the last publication was crash-retracted — see
  // RepairAfterWriterCrash). Never blocks the writer; retries while the
  // writer is mid-store or has lapped the captured cursor (writer
  // progress).
  bool Load(T& out) const noexcept {
    std::uint64_t staged[kWords];
    for (;;) {
      const std::uint64_t c =
          header_->write_count.load(std::memory_order_acquire);       // (R0)
      if (c == 0) {
        return false;  // never written
      }
      const Cell& cell = cells_[(c - 1) % N];
      const std::uint64_t s1 = cell.seq.load(std::memory_order_acquire);
      // ^ (R1)
      if (s1 == 0) {
        return false;  // crash-repaired: the publication was retracted
      }
      if ((s1 & 1u) != 0u) {  // write in progress — retry
        CpuRelax();
        continue;
      }
      const std::uint64_t idx = cell.index.load(std::memory_order_relaxed);
      for (std::size_t i = 0; i < kWords; ++i) {
        staged[i] = cell.words[i].load(std::memory_order_relaxed);
      }
      std::atomic_thread_fence(std::memory_order_acquire);             // (R2)
      if (cell.seq.load(std::memory_order_relaxed) != s1) {
        CpuRelax();  // overwrite raced the copy — retry
        continue;
      }
      if (idx != c - 1) {  // writer lapped: c is stale — reload and retry
        CpuRelax();
        continue;
      }
      break;  // copy validated: no overwrite raced it, indexes match
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
  LoadResult LoadBounded(T& out, std::uint32_t max_retries) const noexcept {
    std::uint64_t staged[kWords];
    for (std::uint32_t attempt = 0; attempt <= max_retries; ++attempt) {
      const std::uint64_t c =
          header_->write_count.load(std::memory_order_acquire);       // (R0)
      if (c == 0) {
        return LoadResult::kEmpty;  // never written
      }
      const Cell& cell = cells_[(c - 1) % N];
      const std::uint64_t s1 = cell.seq.load(std::memory_order_acquire);
      // ^ (R1)
      if (s1 == 0) {
        return LoadResult::kEmpty;  // crash-repaired: publication retracted
      }
      if ((s1 & 1u) != 0u) {  // write in progress — or writer died mid-store
        CpuRelax();
        continue;
      }
      const std::uint64_t idx = cell.index.load(std::memory_order_relaxed);
      for (std::size_t i = 0; i < kWords; ++i) {
        staged[i] = cell.words[i].load(std::memory_order_relaxed);
      }
      std::atomic_thread_fence(std::memory_order_acquire);             // (R2)
      if (cell.seq.load(std::memory_order_relaxed) == s1 && idx == c - 1) {
        std::memcpy(&out, staged, sizeof(T));
        return LoadResult::kValue;
      }
      CpuRelax();  // overwrite or lap raced the copy — bounded retry
    }
    return LoadResult::kStalled;
  }

  // Newest-first non-consuming snapshot of up to k of the last N values
  // into caller-provided storage. Returns the number captured: entries are
  // torn-free with strictly consecutive descending write indexes; a
  // concurrent writer can only shorten the tail (the lap-detection argument
  // in the header comment). Single pass, no retries — wait-free.
  std::size_t Snapshot(T* out, std::size_t k) const noexcept {
    const std::uint64_t c =
        header_->write_count.load(std::memory_order_acquire);         // (R0)
    std::size_t limit = k;
    if (c < limit) {
      limit = static_cast<std::size_t>(c);
    }
    if (N < limit) {
      limit = N;
    }
    std::uint64_t staged[kWords];
    std::size_t taken = 0;
    for (std::size_t i = 0; i < limit; ++i) {
      const std::uint64_t want = c - 1 - i;  // published: want < c
      const Cell& cell = cells_[want % N];
      const std::uint64_t s1 = cell.seq.load(std::memory_order_acquire);
      // ^ (R1)
      if (s1 == 0 || (s1 & 1u) != 0u) {
        break;  // crash-retracted, or a lap is mid-write here — tail ends
      }
      const std::uint64_t idx = cell.index.load(std::memory_order_relaxed);
      for (std::size_t w = 0; w < kWords; ++w) {
        staged[w] = cell.words[w].load(std::memory_order_relaxed);
      }
      std::atomic_thread_fence(std::memory_order_acquire);             // (R2)
      if (cell.seq.load(std::memory_order_relaxed) != s1) {
        break;  // a lap overwrote the cell during the copy — tail ends
      }
      if (idx != want) {
        break;  // validated, but the cell holds a lapped (newer) value
      }
      std::memcpy(out + taken, staged, sizeof(T));
      ++taken;
    }
    return taken;
  }

  // Crash repair (P1b) — wiring path of a (re)claiming WRITER only, while it
  // is provably the ring's sole writer (in xmMessaging: the
  // publisher-liveness claim in shm_segment.hpp). A writer SIGKILLed
  // mid-Store left the IN-FLIGHT cell's (cell write_count % N — the cursor
  // never advanced past it) sequence odd and its words half-written; the
  // in-flight value was never published, so the honest repair is "never
  // written": that cell validates for no reader (staleness keeps rising —
  // M4-A1), older cells stay readable, and the next Store starts a fresh
  // even/odd cycle there.
  void RepairAfterWriterCrash() noexcept {
    Cell& c =
        cells_[header_->write_count.load(std::memory_order_relaxed) % N];
    if ((c.seq.load(std::memory_order_relaxed) & 1u) != 0u) {
      c.seq.store(0, std::memory_order_release);
    }
  }

  // Parking seam (bounded parking verbs only; NEVER touched by
  // Store/Load/Snapshot — see event_count.hpp).
  EventCountPolicy& event_count() noexcept { return event_count_; }

 private:
  static constexpr std::size_t kWords =
      (sizeof(T) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);

  static constexpr std::size_t RoundUpCacheLine(std::size_t bytes) noexcept {
    return (bytes + 63u) & ~static_cast<std::size_t>(63u);
  }

  struct Cell {
    // seq MUST stay the first member: a region-placed ring's first word is
    // pointer-interconvertible with cell 0's sequence (crash tooling and
    // the stalled-writer tests rely on it).
    std::atomic<std::uint64_t> seq;
    std::atomic<std::uint64_t> index;  // write position of the held value
    std::atomic<std::uint64_t> words[kWords];
  };

  struct Header {
    std::atomic<std::uint64_t> write_count;  // the single writer's cursor
  };

  typename Storage::template ArrayHandle<Cell> cells_;
  typename Storage::template SingleHandle<Header> header_;
  EventCountPolicy event_count_;
};

}  // namespace detail
}  // namespace concurrency
}  // namespace xmotion
