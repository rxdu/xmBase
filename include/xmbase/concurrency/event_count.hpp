/*
 * event_count.hpp
 *
 * EventCountPolicy policy: how a thread parks while it waits for progress on a
 * concurrency primitive. Used ONLY by bounded parking verbs (e.g. "wait for
 * work or shutdown", "wait for a reply") and NEVER on a wait-free/lock-free
 * hot path — those verbs do not signal anyone.
 *
 * Promoted verbatim from xmMessaging detail/waiter.hpp (renamed) (ADR 0007, W1);
 * requirement/decision IDs in comments (R1, R8, D10/D11/D16, P0b, P1b) are
 * xmMessaging's, retained so the evidence keeps its provenance.
 *
 * Which waiter to use for timed parks (P0b part 2 decision, recorded):
 * EventCount, NOT CondvarEventCount. Empirically verified on the R1 baseline
 * (GCC 11.4 / Ubuntu 22.04): a timed std::condition_variable wait compiles
 * to pthread_cond_clockwait (CLOCK_MONOTONIC), which that libtsan does NOT
 * intercept — every timed monotonic condvar park produces false "double
 * lock"/data-race reports, and TSan cleanliness is a gate. The futex
 * eventcount below has no such problem: all synchronization is ordinary C++
 * atomics (which TSan models natively); the futex syscall is only a
 * sleep/wake mechanism with no memory-ordering role, and its relative
 * timeout is judged on CLOCK_MONOTONIC by the kernel — the R8 clock,
 * exactly. Any timed wait added on this seam must stay TSan-verifiable on
 * the GCC 11 (Ubuntu 22.04, R1) baseline.
 *
 * This is the second half of the region reuse seam (see storage.hpp): the
 * waiter interface (NotifyAll + predicated bounded wait) is what a shared-
 * memory user reuses — EventCount already IS the shm-capable shape (the
 * P1b variant moves the epoch word into the shared mapping). CondvarEventCount
 * is retained as the portable non-Linux fallback; it becomes usable for
 * timed parks only when the baseline toolchain's TSan intercepts
 * pthread_cond_clockwait (GCC >= 12).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "xmbase/types/time.hpp"

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>
#else
#include <thread>
#endif

namespace xmotion {
namespace concurrency {

// Futex-backed eventcount: the process-private (and shared-mapping) waiter
// for bounded parking verbs. TSan-verifiable on the R1 baseline (see the
// header comment). Protocol (standard eventcount, lost-wakeup-free):
//   waiter:   gen = epoch.load(acquire); if (pred) done;
//             futex_wait(&epoch, gen, remaining)   // EAGAIN if epoch moved
//   notifier: make pred true; epoch.fetch_add(1, release); futex_wake(all)
// A notifier that bumps epoch after the waiter's load makes the wait return
// immediately (value mismatch), so the pred-recheck can never be lost.
class EventCount {
 public:
  // Process-private eventcount: the epoch word is a member and futex ops
  // carry FUTEX_PRIVATE_FLAG (cheaper: no shared-mapping key lookup).
  EventCount() noexcept : word_(&internal_), shared_(false) {}

  // Shared-mapping form (P1b): the eventcount over a 32-bit word living in
  // a SHARED mapping (e.g. a segment header's futex word). Futex ops drop
  // FUTEX_PRIVATE_FLAG so waiters/wakers in other processes attached to the
  // same mapping participate — this is the "designed to be shm-capable"
  // claim made good: same protocol, same code, only the word's home and the
  // futex flags differ. The word's lifetime is the mapping's; this object
  // is a per-process view.
  explicit EventCount(std::atomic<std::uint32_t>* shared_word) noexcept
      : word_(shared_word), shared_(true) {}

  EventCount(const EventCount&) = delete;
  EventCount& operator=(const EventCount&) = delete;

  // Wake every parked thread. Called from transition sites (work becoming
  // pending, a reply landing, a peer detaching) — never from a hot path.
  void NotifyAll() noexcept {
    word_->fetch_add(1, std::memory_order_release);
    Wake();
  }

  // Park until predicate() is true or deadline_at (monotonic, R8) passes.
  // Returns predicate()'s final value. Spurious wakes and EINTR re-check
  // the predicate and re-park on the remaining time — the bound holds.
  template <typename Predicate>
  bool WaitUntil(::xmotion::Timestamp deadline_at, Predicate predicate) {
    for (;;) {
      const std::uint32_t gen = word_->load(std::memory_order_acquire);
      if (predicate()) {
        return true;
      }
      const ::xmotion::Duration remaining = deadline_at - ::xmotion::Now();
      if (remaining <= ::xmotion::Duration::zero()) {
        return predicate();
      }
      WaitOn(gen, remaining);
    }
  }

  // Park for at most max_park (same contract as WaitUntil).
  template <typename Rep, typename Period, typename Predicate>
  bool WaitFor(std::chrono::duration<Rep, Period> max_park,
               Predicate predicate) {
    return WaitUntil(::xmotion::Now() + std::chrono::duration_cast<
                                            ::xmotion::Duration>(max_park),
                     predicate);
  }

 private:
#if defined(__linux__)
  void Wake() noexcept {
    ::syscall(SYS_futex, word_, shared_ ? FUTEX_WAKE : FUTEX_WAKE_PRIVATE,
              INT32_MAX, nullptr, nullptr, 0);
  }

  void WaitOn(std::uint32_t expected, ::xmotion::Duration remaining) noexcept {
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(remaining)
            .count();
    // FUTEX_WAIT relative timeouts are judged on CLOCK_MONOTONIC (R8).
    timespec ts{static_cast<time_t>(ns / 1000000000),
                static_cast<long>(ns % 1000000000)};
    ::syscall(SYS_futex, word_, shared_ ? FUTEX_WAIT : FUTEX_WAIT_PRIVATE,
              expected, &ts, nullptr,
              0);  // EAGAIN/EINTR/ETIMEDOUT all re-enter the caller's loop
  }
#else
  // Portable fallback (non-Linux dev hosts only; the deployed baseline is
  // Linux, R1): bounded sleep-poll, 500 us quantum.
  void Wake() noexcept {}

  void WaitOn(std::uint32_t expected, ::xmotion::Duration remaining) noexcept {
    const auto quantum = std::chrono::microseconds(500);
    std::this_thread::sleep_for(
        remaining < ::xmotion::Duration(quantum)
            ? remaining
            : ::xmotion::Duration(quantum));
    (void)expected;
  }
#endif

  // The futex word: the internal member by default, a word inside a shared
  // mapping for the P1b shared form. 32-bit by kernel contract; the atomic
  // is the ONLY synchronization TSan needs to see (the syscall carries none
  // — note TSan cannot see the CROSS-PROCESS half of a shared-word
  // protocol; the in-process protocol is what it verifies).
  std::atomic<std::uint32_t>* word_;
  std::atomic<std::uint32_t> internal_{0};
  bool shared_;
  static_assert(sizeof(std::atomic<std::uint32_t>) == 4,
                "futex word must be exactly 32 bits");
  static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                "futex word must be address-free for the shared form");
};

// Condition-variable waiter (portable reference; NOT used for timed parks on
// the R1 baseline — see the header comment for the TSan evidence).
class CondvarEventCount {
 public:
  // Wake every parked thread. Called from wiring-time paths only.
  void NotifyAll() noexcept {
    {
      // Empty critical section: pairs the notify with a parked thread that
      // is between its predicate check and its wait (the classic lost-wakeup
      // window).
      std::lock_guard<std::mutex> lock(mutex_);
    }
    cv_.notify_all();
  }

  // Park for at most max_park or until predicate() becomes true after a
  // notification. Returns predicate()'s final value (condvar semantics).
  template <typename Rep, typename Period, typename Predicate>
  bool WaitFor(std::chrono::duration<Rep, Period> max_park,
               Predicate predicate) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, max_park, predicate);
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
};

}  // namespace concurrency
}  // namespace xmotion
