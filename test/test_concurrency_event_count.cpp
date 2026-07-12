/*
 * test_concurrency_waiter.cpp — the Waiter policies, verified where the
 * code now lives (ADR 0007 W1).
 *
 * The lost-wakeup hammer is the load-bearing test: it repeatedly races the
 * notifier against the waiter's check-then-park window; the eventcount
 * protocol (epoch bump before wake) must make every round observe the
 * predicate — a lost wakeup shows up as a timeout. Runs under TSan in CI.
 *
 * The CondvarEventCount timed-park test is skipped under TSan: on the GCC 11
 * (Ubuntu 22.04, R1) baseline libtsan does not intercept
 * pthread_cond_clockwait, producing false reports (the recorded evidence in
 * event_count.hpp — the reason EventCount exists).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "xmbase/concurrency/event_count.hpp"

#if defined(__SANITIZE_THREAD__)
#define XMBASE_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define XMBASE_TSAN 1
#endif
#endif

namespace {

using namespace std::chrono_literals;
using xmotion::concurrency::CondvarEventCount;
using xmotion::concurrency::EventCount;

TEST(ConcurrencyWaiter, FutexWaitReturnsImmediatelyWhenPredicateHolds) {
  EventCount waiter;
  const auto t0 = xmotion::Now();
  EXPECT_TRUE(waiter.WaitFor(5s, [] { return true; }));
  EXPECT_LT(xmotion::Now() - t0, 1s);  // did not park for the full bound
}

TEST(ConcurrencyWaiter, FutexWaitTimesOutOnFalsePredicate) {
  EventCount waiter;
  const auto t0 = xmotion::Now();
  EXPECT_FALSE(waiter.WaitFor(50ms, [] { return false; }));
  // The bound holds: the park lasted at least ~the requested time.
  EXPECT_GE(xmotion::Now() - t0, 40ms);
}

TEST(ConcurrencyWaiter, FutexWaitUntilPastDeadlineReturnsPredicate) {
  EventCount waiter;
  EXPECT_FALSE(waiter.WaitUntil(xmotion::Now() - 1ms, [] { return false; }));
  EXPECT_TRUE(waiter.WaitUntil(xmotion::Now() - 1ms, [] { return true; }));
}

// The lost-wakeup hammer: many rounds of notifier racing the waiter's
// check-then-park window. The eventcount protocol makes the wait return
// immediately when the epoch moved between the waiter's load and its park —
// a lost wakeup would surface as a round that burns the full 5 s bound.
TEST(ConcurrencyWaiter, FutexLostWakeupHammer) {
  constexpr int kRounds = 300;
  EventCount waiter;
  for (int round = 0; round < kRounds; ++round) {
    std::atomic<bool> flag{false};
    std::thread notifier([&] {
      flag.store(true, std::memory_order_release);
      waiter.NotifyAll();
    });
    const bool woke = waiter.WaitFor(
        5s, [&] { return flag.load(std::memory_order_acquire); });
    notifier.join();
    ASSERT_TRUE(woke) << "lost wakeup at round " << round;
  }
}

TEST(ConcurrencyWaiter, FutexNotifyAllWakesEveryWaiter) {
  EventCount waiter;
  std::atomic<bool> flag{false};
  std::atomic<int> woke{0};
  std::vector<std::thread> waiters;
  for (int i = 0; i < 4; ++i) {
    waiters.emplace_back([&] {
      if (waiter.WaitFor(
              5s, [&] { return flag.load(std::memory_order_acquire); })) {
        woke.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  std::this_thread::sleep_for(20ms);  // let them park
  flag.store(true, std::memory_order_release);
  waiter.NotifyAll();
  for (auto& t : waiters) {
    t.join();
  }
  EXPECT_EQ(woke.load(), 4);
}

// The shared-word form (P1b shape): two per-process views over ONE epoch
// word — the same protocol xmMessaging runs across processes, verified here
// across threads (the half TSan can see).
TEST(ConcurrencyWaiter, FutexSharedWordFormWakesAcrossViews) {
  std::atomic<std::uint32_t> shared_word{0};
  EventCount view_a(&shared_word);
  EventCount view_b(&shared_word);

  std::atomic<bool> flag{false};
  std::thread parked([&] {
    EXPECT_TRUE(view_a.WaitFor(
        5s, [&] { return flag.load(std::memory_order_acquire); }));
  });
  std::this_thread::sleep_for(20ms);
  flag.store(true, std::memory_order_release);
  view_b.NotifyAll();  // the OTHER view wakes the parked one
  parked.join();
}

#if !defined(XMBASE_TSAN)
TEST(ConcurrencyWaiter, CondvarEventCountNotifyAndTimeout) {
  CondvarEventCount waiter;
  std::atomic<bool> flag{false};
  std::thread parked([&] {
    EXPECT_TRUE(waiter.WaitFor(
        5s, [&] { return flag.load(std::memory_order_acquire); }));
  });
  std::this_thread::sleep_for(20ms);
  flag.store(true, std::memory_order_release);
  waiter.NotifyAll();
  parked.join();

  EXPECT_FALSE(waiter.WaitFor(30ms, [] { return false; }));
}
#endif

}  // namespace
