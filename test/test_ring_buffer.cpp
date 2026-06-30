/*
 * test_ring_buffer.cpp — properties of the general-purpose RingBuffer.
 *
 * Covers the core contract plus the correctness properties that the hardened
 * version guarantees: overwrite keeps occupancy/full/empty accounting correct
 * across many laps, PeekAt rejects out-of-range indices, and Peek/Read clamp to
 * the caller's buffer (never OOB).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <vector>

#include "gtest/gtest.h"

#include "xmsigma/container/ring_buffer.hpp"

using xmotion::RingBuffer;

TEST(RingBufferTest, BasicFifoNoOverwrite) {
  RingBuffer<int, 8> rb(/*enable_overwrite=*/false);
  EXPECT_TRUE(rb.IsEmpty());
  for (int i = 0; i < 7; ++i) EXPECT_EQ(rb.Write(i), 1u);  // 7 usable slots
  EXPECT_TRUE(rb.IsFull());
  EXPECT_EQ(rb.Write(99), 0u);  // full, overwrite disabled -> rejected
  int v;
  for (int i = 0; i < 7; ++i) {
    EXPECT_EQ(rb.Read(v), 1u);
    EXPECT_EQ(v, i);
  }
  EXPECT_TRUE(rb.IsEmpty());
  EXPECT_EQ(rb.Read(v), 0u);
}

TEST(RingBufferTest, OverwriteKeepsAccountingCorrectAcrossLaps) {
  RingBuffer<int, 4> rb(true);  // 3 usable slots
  for (int i = 0; i < 100; ++i) rb.Write(i);
  EXPECT_EQ(rb.GetOccupiedSize(), 3u);
  EXPECT_EQ(rb.GetFreeSize(), 0u);
  EXPECT_TRUE(rb.IsFull());
  EXPECT_FALSE(rb.IsEmpty());
  int v;
  EXPECT_EQ(rb.Read(v), 1u);
  EXPECT_EQ(v, 97);  // three newest survive, FIFO
  EXPECT_EQ(rb.Read(v), 1u);
  EXPECT_EQ(v, 98);
  EXPECT_EQ(rb.Read(v), 1u);
  EXPECT_EQ(v, 99);
  EXPECT_TRUE(rb.IsEmpty());
}

TEST(RingBufferTest, PeekAtRejectsOutOfRangeIndex) {
  RingBuffer<int, 8> rb(false);
  rb.Write(10);
  rb.Write(20);
  int v;
  EXPECT_EQ(rb.PeekAt(v, 0), 1u);
  EXPECT_EQ(v, 10);
  EXPECT_EQ(rb.PeekAt(v, 1), 1u);
  EXPECT_EQ(v, 20);
  EXPECT_EQ(rb.PeekAt(v, 2), 0u);   // exactly at availability
  EXPECT_EQ(rb.PeekAt(v, 99), 0u);  // far beyond
}

TEST(RingBufferTest, PeekClampsAndDoesNotConsume) {
  RingBuffer<int, 8> rb(false);
  for (int i = 0; i < 5; ++i) rb.Write(i);

  std::vector<int> small(2);
  EXPECT_EQ(rb.Peek(small, 100), 2u);  // clamped to data.size()
  EXPECT_EQ(small[0], 0);
  EXPECT_EQ(small[1], 1);

  std::vector<int> big(10);
  EXPECT_EQ(rb.Peek(big, 10), 5u);      // clamped to availability
  EXPECT_EQ(rb.GetOccupiedSize(), 5u);  // not consumed
}

TEST(RingBufferTest, BurstReadWriteClampToCallerBuffer) {
  RingBuffer<int, 16> rb(false);
  std::vector<int> in = {1, 2, 3, 4, 5};
  EXPECT_EQ(rb.Write(in, 100), 5u);  // clamped to new_data.size()
  std::vector<int> out(3);
  EXPECT_EQ(rb.Read(out, 100), 3u);  // clamped to data.size()
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[2], 3);
  EXPECT_EQ(rb.GetOccupiedSize(), 2u);
}
