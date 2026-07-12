/*
 * test_concurrency_message_slot.cpp — the seqlock MessageSlot contract,
 * verified where the code now lives (ADR 0007 W1).
 *
 * The tear check runs the Boehm-seqlock claim under real concurrent load
 * with a checksummed payload: a torn read that validates would fail the
 * checksum. This suite is part of the TSan CI leg (suite names carry
 * "Concurrency" for the ctest filter).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "xmbase/concurrency/message_slot.hpp"

namespace {

using xmotion::concurrency::EventCount;
using xmotion::concurrency::HeapStorage;
using xmotion::concurrency::MessageSlot;
using xmotion::concurrency::MutexMessageSlot;
using xmotion::concurrency::RegionStorage;

// Checksummed payload: every word is a function of the seed; a torn read
// (words from two different Store calls) cannot produce a valid checksum.
struct Checksummed {
  std::uint64_t seed = 0;
  std::uint64_t words[14] = {};
  std::uint64_t checksum = 0;

  static Checksummed Make(std::uint64_t seed) {
    Checksummed c;
    c.seed = seed;
    std::uint64_t sum = seed;
    for (std::uint64_t i = 0; i < 14; ++i) {
      c.words[i] = seed * 0x9E3779B97F4A7C15ull + i;
      sum ^= c.words[i];
    }
    c.checksum = sum;
    return c;
  }

  bool Valid() const {
    std::uint64_t sum = seed;
    for (std::uint64_t i = 0; i < 14; ++i) {
      sum ^= words[i];
    }
    return sum == checksum;
  }
};
static_assert(std::is_trivially_copyable_v<Checksummed>,
              "tear-check payload must be seqlock-storable");

TEST(ConcurrencyMessageSlot, NeverWrittenLoadsNothing) {
  MessageSlot<Checksummed> slot;
  Checksummed out;
  EXPECT_FALSE(slot.Load(out));
  EXPECT_EQ(slot.LoadBounded(out, 8),
            (MessageSlot<Checksummed>::LoadResult::kEmpty));
}

TEST(ConcurrencyMessageSlot, StoreThenLoadRoundTrips) {
  MessageSlot<Checksummed> slot;
  slot.Store(Checksummed::Make(42));
  Checksummed out;
  ASSERT_TRUE(slot.Load(out));
  EXPECT_EQ(out.seed, 42u);
  EXPECT_TRUE(out.Valid());
  EXPECT_EQ(slot.LoadBounded(out, 8),
            (MessageSlot<Checksummed>::LoadResult::kValue));
  EXPECT_TRUE(out.Valid());
}

// The tear check: one writer overwriting continuously, several readers
// validating every copy they get. Any torn-but-validated read fails the
// checksum. Runs under TSan in CI (the Boehm construction must be
// data-race-free through the atomic word array).
TEST(ConcurrencyMessageSlot, ConcurrentOverwriteNeverTears) {
  MessageSlot<Checksummed> slot;
// Sanitized builds instrument every atomic op (~10-15x): a smaller store
// count keeps the TSan CI lane around a second while the plain build gets
// a longer racing window.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
  constexpr int kStores = 100000;
#else
  constexpr int kStores = 2000000;
#endif
  constexpr int kReaders = 4;

  std::atomic<bool> writer_done{false};
  std::atomic<std::uint64_t> torn{0};
  std::atomic<std::uint64_t> loads{0};

  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&] {
      Checksummed out;
      std::uint64_t last_seed = 0;
      std::uint64_t local_loads = 0;
      while (!writer_done.load(std::memory_order_acquire)) {
        if (slot.Load(out)) {
          ++local_loads;
          if (!out.Valid()) {
            torn.fetch_add(1, std::memory_order_relaxed);
          }
          // Latest-value-wins: seeds may repeat but never go backwards.
          if (out.seed < last_seed) {
            torn.fetch_add(1, std::memory_order_relaxed);
          }
          last_seed = out.seed;
        }
      }
      // Post-quiescence load: guaranteed to validate (writer done), so the
      // liveness assertion below cannot flake under serializing schedulers
      // (valgrind runs readers only after the writer finishes its quantum).
      if (slot.Load(out)) {
        ++local_loads;
        if (!out.Valid() || out.seed < last_seed) {
          torn.fetch_add(1, std::memory_order_relaxed);
        }
      }
      loads.fetch_add(local_loads, std::memory_order_relaxed);
    });
  }

  for (int i = 1; i <= kStores; ++i) {
    slot.Store(Checksummed::Make(static_cast<std::uint64_t>(i)));
  }
  writer_done.store(true, std::memory_order_release);
  for (auto& t : readers) {
    t.join();
  }

  EXPECT_EQ(torn.load(), 0u);
  EXPECT_GT(loads.load(), 0u);

  Checksummed out;
  ASSERT_TRUE(slot.Load(out));
  EXPECT_EQ(out.seed, static_cast<std::uint64_t>(kStores));
  EXPECT_TRUE(out.Valid());
}

// A writer that dies mid-Store leaves the sequence odd. LoadBounded must
// DETECT the stall (never spin forever), and the (re)claiming writer's
// repair must return the slot to "never written". The mid-Store state is
// crafted through a region placement: seq is the cell's first member, so
// the region's first word is pointer-interconvertible with it.
TEST(ConcurrencyMessageSlot, LoadBoundedDetectsStalledWriterAndRepairs) {
  using Slot = MessageSlot<Checksummed, RegionStorage, EventCount>;
  alignas(64) unsigned char region[Slot::StorageBytes() + 64] = {};

  Slot writer_view(RegionStorage(region, sizeof(region), true));
  writer_view.Store(Checksummed::Make(7));

  Slot reader_view(RegionStorage(region, sizeof(region), false));
  Checksummed out;
  ASSERT_EQ(reader_view.LoadBounded(out, 8), Slot::LoadResult::kValue);
  EXPECT_EQ(out.seed, 7u);

  // Simulate SIGKILL mid-Store: bump the sequence to odd, words half-baked.
  auto* seq = reinterpret_cast<std::atomic<std::uint64_t>*>(region);
  seq->store(seq->load(std::memory_order_relaxed) + 1,
             std::memory_order_release);

  EXPECT_EQ(reader_view.LoadBounded(out, 64), Slot::LoadResult::kStalled);

  // The reclaiming sole writer repairs to "never written"...
  writer_view.RepairAfterWriterCrash();
  EXPECT_EQ(reader_view.LoadBounded(out, 8), Slot::LoadResult::kEmpty);
  EXPECT_FALSE(reader_view.Load(out));

  // ...and the next Store starts a fresh cycle.
  writer_view.Store(Checksummed::Make(8));
  ASSERT_EQ(reader_view.LoadBounded(out, 8), Slot::LoadResult::kValue);
  EXPECT_EQ(out.seed, 8u);
  EXPECT_TRUE(out.Valid());
}

// MutexMessageSlot is the fallback for non-trivially-copyable values; for
// values BOTH slots accept, the observable contract must be equivalent.
TEST(ConcurrencyMessageSlot, MutexFallbackMatchesSeqlockSemantics) {
  MessageSlot<Checksummed> seqlock_slot;
  MutexMessageSlot<Checksummed> mutex_slot;

  Checksummed a, b;
  EXPECT_EQ(seqlock_slot.Load(a), mutex_slot.Load(b));  // both empty

  for (std::uint64_t seed = 1; seed <= 32; ++seed) {
    const Checksummed value = Checksummed::Make(seed);
    seqlock_slot.Store(value);
    mutex_slot.Store(value);
    ASSERT_TRUE(seqlock_slot.Load(a));
    ASSERT_TRUE(mutex_slot.Load(b));
    EXPECT_EQ(std::memcmp(&a, &b, sizeof(a)), 0);  // identical bytes
    EXPECT_TRUE(a.Valid());
  }
}

// The reason MutexMessageSlot exists: values the seqlock cannot store.
TEST(ConcurrencyMessageSlot, MutexFallbackCarriesRichTypes) {
  MutexMessageSlot<std::string> slot;
  std::string out;
  EXPECT_FALSE(slot.Load(out));
  slot.Store(std::string("first"));
  slot.Store(std::string("latest value wins"));
  ASSERT_TRUE(slot.Load(out));
  EXPECT_EQ(out, "latest value wins");
}

}  // namespace
