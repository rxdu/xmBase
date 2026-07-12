/*
 * test_concurrency_spsc_queue.cpp — the SPSC SpscQueue contract,
 * verified where the code now lives (ADR 0007 W1).
 *
 * The conservation stress is the load-bearing test: across a real
 * producer/consumer thread pair, every pushed value arrives exactly once,
 * in order, with nothing invented and nothing lost. Runs under TSan in CI.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <atomic>
#include <cstdint>
#include <thread>

#include "gtest/gtest.h"
#include "xmbase/concurrency/spsc_queue.hpp"

namespace {

using xmotion::concurrency::SpscQueue;
using xmotion::concurrency::HeapStorage;

struct Record {
  std::uint64_t sequence = 0;
  std::uint64_t payload[7] = {};
};

TEST(ConcurrencySpscQueue, EmptyPopsNothing) {
  SpscQueue<Record> queue(4);
  Record out;
  EXPECT_FALSE(queue.TryPop(out));
  EXPECT_EQ(queue.Size(), 0u);
  EXPECT_EQ(queue.capacity(), 4u);
  EXPECT_FALSE(queue.Full());
}

TEST(ConcurrencySpscQueue, ZeroDepthClampsToOne) {
  SpscQueue<Record> queue(0);
  EXPECT_EQ(queue.capacity(), 1u);
  Record r;
  r.sequence = 1;
  EXPECT_TRUE(queue.TryPush(r));
  EXPECT_TRUE(queue.Full());
  EXPECT_FALSE(queue.TryPush(r));  // full: refused, nothing enqueued
  Record out;
  ASSERT_TRUE(queue.TryPop(out));
  EXPECT_EQ(out.sequence, 1u);
  EXPECT_FALSE(queue.TryPop(out));
}

TEST(ConcurrencySpscQueue, FifoOrderAndFullRefusal) {
  SpscQueue<Record> queue(3);
  Record r;
  for (std::uint64_t i = 1; i <= 3; ++i) {
    r.sequence = i;
    EXPECT_TRUE(queue.TryPush(r));
  }
  EXPECT_TRUE(queue.Full());
  r.sequence = 4;
  EXPECT_FALSE(queue.TryPush(r));
  EXPECT_EQ(queue.Size(), 3u);

  Record out;
  for (std::uint64_t i = 1; i <= 3; ++i) {
    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(out.sequence, i);
  }
  EXPECT_FALSE(queue.TryPop(out));
}

// Conservation under a real SPSC thread pair: N pushed values arrive
// exactly once, strictly in order. The producer spins on Full (the caller's
// back-pressure policy); the consumer polls until it has everything.
TEST(ConcurrencySpscQueue, SpscConservationStress) {
// Sanitized builds instrument every atomic op: a smaller count keeps the
// sanitizer CI lanes fast while the plain build gets a longer racing window.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
  constexpr std::uint64_t kCount = 200000;
#else
  constexpr std::uint64_t kCount = 2000000;
#endif
  SpscQueue<Record> queue(64);

  std::atomic<bool> failed{false};
  std::thread consumer([&] {
    Record out;
    std::uint64_t expected = 1;
    std::uint64_t checksum = 0;
    while (expected <= kCount) {
      if (queue.TryPop(out)) {
        if (out.sequence != expected ||
            out.payload[0] != out.sequence * 3u) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
        checksum += out.sequence;
        ++expected;
      }
    }
    // Sum of 1..N: nothing lost, nothing duplicated, nothing invented.
    if (checksum != kCount * (kCount + 1) / 2) {
      failed.store(true, std::memory_order_relaxed);
    }
  });

  Record r;
  for (std::uint64_t i = 1; i <= kCount; ++i) {
    r.sequence = i;
    r.payload[0] = i * 3u;
    while (!queue.TryPush(r)) {
      std::this_thread::yield();  // back-pressure: wait for the consumer
    }
  }
  consumer.join();

  EXPECT_FALSE(failed.load());
  Record out;
  EXPECT_FALSE(queue.TryPop(out));  // fully drained
  EXPECT_EQ(queue.Size(), 0u);
}

}  // namespace
