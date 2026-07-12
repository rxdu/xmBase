/*
 * spsc_queue.hpp
 *
 * SpscQueue<T, Storage> — a lock-free bounded SPSC ring (lock-free,
 * allocation-free after wiring, memory sized at wiring from the declared
 * depth — R7).
 *
 * Promoted verbatim from xmMessaging detail/spsc_queue.hpp (ADR 0007,
 * W1); requirement/decision IDs in comments (R7, D7/D8, D15, P0b, P1b) are
 * xmMessaging's, retained so the proofs keep their provenance.
 *
 * Concurrency contract: SPSC — one producer, one consumer. Callers with
 * more than one producer serialize the producer side one level up (that is
 * what xmMessaging's shared-ownership writer mutex does — a deliberate P0b
 * interim):
 *
 *   TODO(P1): lock-free MPMC (or at least MPSC producer side) upgrade for
 *   multi-producer callers. The single-producer path never takes a lock —
 *   any mutex lives with the caller, so the default hot path does not
 *   silently ship a lock.
 *
 * Memory-ordering pairs, referenced from the code:
 *   (Q1) producer: head_.load(acquire)  — pairs with (Q2), the consumer's
 *        head_.store(release) after it copied a cell out: the producer may
 *        only reuse a cell after it has observed the consumer's release,
 *        which orders the consumer's plain copy-out before the producer's
 *        plain overwrite (no data race on cell reuse).
 *   (Q3) producer: tail_.store(release) after the plain cell write — pairs
 *        with (Q4), the consumer's tail_.load(acquire): a consumer that
 *        observes the new tail observes the fully-written cell (no torn
 *        reads; cells need no atomics, unlike the seqlock slot).
 *
 * Overflow policy lives with the caller: TryPush returns false when full
 * and enqueues nothing — the caller decides whether that is back-pressure
 * (refuse), drop-newest (count and continue), or something else.
 *
 * T: any copyable type works with HeapStorage (the Q1–Q4 ordering makes
 * the plain cell copies race-free). RegionStorage additionally requires
 * trivially copyable / trivially destructible T (cells living in a shared
 * region are never destroyed and are copied across process boundaries).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "xmbase/concurrency/storage.hpp"

namespace xmotion {
namespace concurrency {

template <typename T, typename Storage = HeapStorage>
class SpscQueue {
 public:
  // Ring control block. Placed like the cells (heap or a shared region,
  // P1b): producer and consumer may live in different PROCESSES, so the
  // indices — and the capacity, which the initializing side (the consumer,
  // who declared the depth) authors — must live in the placed region, not
  // as members of this per-process view object.
  struct Control {
    std::atomic<std::uint64_t> capacity;
    // Separate cache lines: producer writes tail, consumer writes head.
    alignas(64) std::atomic<std::uint64_t> head;
    alignas(64) std::atomic<std::uint64_t> tail;
  };
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "region-capable ring requires lock-free (address-free) "
                "64-bit atomics");

  // Placed-storage sizes for a region-layout computation (P1b).
  static constexpr std::size_t ControlBytes() noexcept {
    return sizeof(Control);
  }
  static constexpr std::size_t RecordBytes() noexcept { return sizeof(T); }

  // Depth comes from the caller's wiring-time declaration; all cell memory
  // is allocated/claimed here, never on the push/pop path (R7). Only an
  // INITIALIZING storage authors capacity/indices; an attaching view
  // (P1b: the producer's view of a consumer-owned ring) passes the region's
  // cell bound as `depth` and reads the live capacity from the shared
  // control block.
  explicit SpscQueue(std::uint32_t depth, Storage storage = Storage())
      : ctrl_(storage.template MakeSingle<Control>()),
        cells_(storage.template MakeArray<T>(depth == 0 ? 1 : depth)) {
    if (storage.Initialize()) {
      ctrl_->capacity.store(depth == 0 ? 1 : depth, std::memory_order_relaxed);
      ctrl_->head.store(0, std::memory_order_relaxed);
      ctrl_->tail.store(0, std::memory_order_relaxed);
    }
  }

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  // Producer side. Returns false when full (caller applies its policy).
  bool TryPush(const T& value) {
    const std::uint64_t capacity =
        ctrl_->capacity.load(std::memory_order_relaxed);
    const std::uint64_t tail = ctrl_->tail.load(std::memory_order_relaxed);
    const std::uint64_t head =
        ctrl_->head.load(std::memory_order_acquire);                // (Q1)
    if (tail - head >= capacity) {
      return false;
    }
    cells_[tail % capacity] = value;                // plain write, see (Q3)
    ctrl_->tail.store(tail + 1, std::memory_order_release);         // (Q3)
    return true;
  }

  // Consumer side. Returns false when empty.
  bool TryPop(T& out) {
    const std::uint64_t capacity =
        ctrl_->capacity.load(std::memory_order_relaxed);
    const std::uint64_t head = ctrl_->head.load(std::memory_order_relaxed);
    const std::uint64_t tail =
        ctrl_->tail.load(std::memory_order_acquire);                // (Q4)
    if (head == tail) {
      return false;
    }
    out = cells_[head % capacity];                  // plain read, see (Q1)
    ctrl_->head.store(head + 1, std::memory_order_release);         // (Q2)
    return true;
  }

  // Producer-side fullness check (exact from the producer thread: only the
  // consumer can make it less full between this check and TryPush).
  bool Full() const noexcept {
    return ctrl_->tail.load(std::memory_order_relaxed) -
               ctrl_->head.load(std::memory_order_acquire) >=
           ctrl_->capacity.load(std::memory_order_relaxed);
  }

  // Advisory size (for depth gauges).
  std::size_t Size() const noexcept {
    const std::uint64_t head = ctrl_->head.load(std::memory_order_relaxed);
    const std::uint64_t tail = ctrl_->tail.load(std::memory_order_relaxed);
    return tail >= head ? static_cast<std::size_t>(tail - head) : 0;
  }

  std::size_t capacity() const noexcept {
    return static_cast<std::size_t>(
        ctrl_->capacity.load(std::memory_order_relaxed));
  }

 private:
  typename Storage::template SingleHandle<Control> ctrl_;
  typename Storage::template ArrayHandle<T> cells_;
};

}  // namespace concurrency
}  // namespace xmotion
