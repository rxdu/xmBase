/*
 * test_concurrency_message_buffer.cpp — the depth-N seqlock ring contract
 * (docs/concurrency.md): keep the last N; newest-first non-consuming
 * snapshots; writer laps invalidate only the tail of a snapshot.
 *
 * The lap stress runs the Snapshot claim under a max-rate writer with a
 * checksummed payload: every returned snapshot must be internally
 * consistent (torn-free entries, strictly consecutive descending write
 * positions), and the tail-shortening mechanism must be OBSERVED to fire.
 * This suite is part of the TSan CI leg (suite names carry "Concurrency"
 * for the ctest filter).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <type_traits>
#include <vector>

#include "gtest/gtest.h"
#include "xmbase/concurrency/message_buffer.hpp"
#include "xmbase/concurrency/message_slot.hpp"

namespace {

using xmotion::concurrency::EventCount;
using xmotion::concurrency::HeapStorage;
using xmotion::concurrency::MessageBuffer;
using xmotion::concurrency::MessageSlot;
using xmotion::concurrency::RegionStorage;

// Checksummed payload (same shape as the MessageSlot tear check): every
// word is a function of the seed; a torn read cannot produce a valid
// checksum, and the seed doubles as the write position (seed = position+1)
// so snapshots can prove strict consecutive descending order.
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
              "stress payload must be seqlock-storable");

TEST(ConcurrencyMessageBuffer, NewestFirstSnapshotOrderingAndCount) {
  MessageBuffer<Checksummed, 4> hist;
  Checksummed out[4];

  EXPECT_EQ(hist.Snapshot(out, 4), 0u);  // never written

  for (std::uint64_t seed = 1; seed <= 10; ++seed) {
    hist.Store(Checksummed::Make(seed));
  }

  // Full-depth snapshot: the last 4 values, newest first.
  ASSERT_EQ(hist.Snapshot(out, 4), 4u);
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(out[i].seed, 10u - i);
    EXPECT_TRUE(out[i].Valid());
  }

  // k smaller than the depth: the newest k only.
  ASSERT_EQ(hist.Snapshot(out, 2), 2u);
  EXPECT_EQ(out[0].seed, 10u);
  EXPECT_EQ(out[1].seed, 9u);

  // k larger than the depth: capped at N.
  Checksummed big[8];
  EXPECT_EQ(hist.Snapshot(big, 8), 4u);
  EXPECT_EQ(big[0].seed, 10u);
  EXPECT_EQ(big[3].seed, 7u);
}

TEST(ConcurrencyMessageBuffer, SnapshotShorterWhenFewerStored) {
  MessageBuffer<Checksummed, 8> hist;
  Checksummed out[8];

  for (std::uint64_t seed = 1; seed <= 3; ++seed) {
    hist.Store(Checksummed::Make(seed));
  }

  ASSERT_EQ(hist.Snapshot(out, 8), 3u);  // only 3 ever stored
  EXPECT_EQ(out[0].seed, 3u);
  EXPECT_EQ(out[1].seed, 2u);
  EXPECT_EQ(out[2].seed, 1u);

  EXPECT_EQ(hist.Snapshot(out, 0), 0u);  // k == 0 is a no-op
}

TEST(ConcurrencyMessageBuffer, LoadLatestMatchesNewestSnapshotEntry) {
  MessageBuffer<Checksummed, 4> hist;
  Checksummed latest;
  EXPECT_FALSE(hist.LoadLatest(latest));

  for (std::uint64_t seed = 1; seed <= 6; ++seed) {
    hist.Store(Checksummed::Make(seed));
  }

  ASSERT_TRUE(hist.LoadLatest(latest));
  Checksummed out[4];
  ASSERT_GE(hist.Snapshot(out, 4), 1u);
  EXPECT_EQ(latest.seed, out[0].seed);
  EXPECT_EQ(latest.seed, 6u);
}

// N=1 MessageBuffer is MessageSlot over the same core: the observable
// depth-1 contract must be equivalent, verb for verb.
TEST(ConcurrencyMessageBuffer, DepthOneBehavesAsMessageSlot) {
  MessageSlot<Checksummed> slot;
  MessageBuffer<Checksummed, 1> hist;

  Checksummed a, b;
  EXPECT_EQ(slot.Load(a), hist.LoadLatest(b));  // both empty
  EXPECT_EQ(slot.LoadBounded(a, 8),
            (MessageSlot<Checksummed>::LoadResult::kEmpty));
  EXPECT_EQ(hist.LoadLatestBounded(b, 8),
            (MessageBuffer<Checksummed, 1>::LoadResult::kEmpty));
  EXPECT_EQ(hist.Snapshot(&b, 1), 0u);

  for (std::uint64_t seed = 1; seed <= 32; ++seed) {
    const Checksummed value = Checksummed::Make(seed);
    slot.Store(value);
    hist.Store(value);
    ASSERT_TRUE(slot.Load(a));
    ASSERT_TRUE(hist.LoadLatest(b));
    EXPECT_EQ(std::memcmp(&a, &b, sizeof(a)), 0);  // identical bytes
    ASSERT_EQ(hist.Snapshot(&b, 1), 1u);           // depth 1: one entry max
    EXPECT_EQ(b.seed, seed);
  }
}

// The lap stress: a max-rate writer while readers Snapshot continuously.
// Every returned snapshot must be internally consistent — checksummed
// entries (torn-free) with strictly consecutive descending seeds (the
// write-index guarantee, seed = position+1) — and tail-shortening (a
// snapshot cut short by a concurrent lap) must fire at least once: that is
// the mechanism under test, not a tolerated failure mode.
TEST(ConcurrencyMessageBuffer, WriterLapShortensOnlyTheTail) {
  constexpr std::size_t kDepth = 4;
  MessageBuffer<Checksummed, kDepth> hist;
// Sanitized builds instrument every atomic op (~10-15x): a smaller store
// count keeps the TSan CI lane fast while the plain build gets a longer
// racing window (same scaling as the MessageSlot tear check).
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
  constexpr std::uint64_t kStores = 100000;
#else
  constexpr std::uint64_t kStores = 2000000;
#endif
  constexpr int kReaders = 2;

  std::atomic<bool> writer_done{false};
  std::atomic<std::uint64_t> inconsistent{0};
  std::atomic<std::uint64_t> snapshots{0};
  std::atomic<std::uint64_t> shortened{0};

  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&] {
      Checksummed out[kDepth];
      std::uint64_t last_top = 0;
      std::uint64_t local_snapshots = 0;
      std::uint64_t local_shortened = 0;
      std::uint64_t local_inconsistent = 0;
      while (!writer_done.load(std::memory_order_acquire)) {
        const std::size_t n = hist.Snapshot(out, kDepth);
        if (n == 0) {
          continue;
        }
        ++local_snapshots;
        for (std::size_t i = 0; i < n; ++i) {
          if (!out[i].Valid()) {
            ++local_inconsistent;  // torn entry
          }
          // Strictly consecutive descending write positions.
          if (out[i].seed != out[0].seed - i) {
            ++local_inconsistent;
          }
        }
        // Latest-wins: the newest entry never goes backwards.
        if (out[0].seed < last_top) {
          ++local_inconsistent;
        }
        last_top = out[0].seed;
        // Tail shortening: the full depth existed (>= kDepth values had
        // been stored when the cursor was captured) yet the walk was cut
        // short — only a concurrent lap can do that.
        if (n < kDepth && out[0].seed >= kDepth) {
          ++local_shortened;
        }
      }
      inconsistent.fetch_add(local_inconsistent, std::memory_order_relaxed);
      snapshots.fetch_add(local_snapshots, std::memory_order_relaxed);
      shortened.fetch_add(local_shortened, std::memory_order_relaxed);
    });
  }

  for (std::uint64_t seed = 1; seed <= kStores; ++seed) {
    hist.Store(Checksummed::Make(seed));
  }
  writer_done.store(true, std::memory_order_release);
  for (auto& t : readers) {
    t.join();
  }

  EXPECT_EQ(inconsistent.load(), 0u);
  EXPECT_GT(snapshots.load(), 0u);
  EXPECT_GE(shortened.load(), 1u);  // the mechanism must be seen firing

  // Quiescent: the full history is the last kDepth stores, newest first.
  Checksummed out[kDepth];
  ASSERT_EQ(hist.Snapshot(out, kDepth), kDepth);
  for (std::size_t i = 0; i < kDepth; ++i) {
    EXPECT_EQ(out[i].seed, kStores - i);
    EXPECT_TRUE(out[i].Valid());
  }
}

// Depth-N crash story (the LoadBounded poison scenario, generalized): a
// writer SIGKILLed mid-Store poisons only the IN-FLIGHT cell. The latest
// PUBLISHED value stays readable (unlike depth-1, where the in-flight cell
// is the only cell), a snapshot loses only the tail entry whose cell the
// dead writer held, and the reclaiming writer's repair retracts just that
// cell. Crafted through a region placement: cells sit at the region base
// with stride CellBytes(), seq first in each cell (the documented carve).
TEST(ConcurrencyMessageBuffer, CrashedWriterPoisonsOnlyTheInFlightCell) {
  using Hist = MessageBuffer<Checksummed, 2, RegionStorage, EventCount>;
  alignas(64) unsigned char region[Hist::StorageBytes() + 64] = {};

  Hist writer_view(RegionStorage(region, sizeof(region), true));
  // Positions 0,1,2 -> cells 0,1,0: cell 0 holds seed 3 (newest), cell 1
  // holds seed 2; the next store (position 3) would claim cell 1.
  for (std::uint64_t seed = 1; seed <= 3; ++seed) {
    writer_view.Store(Checksummed::Make(seed));
  }

  Hist reader_view(RegionStorage(region, sizeof(region), false));
  Checksummed out[2];
  ASSERT_EQ(reader_view.Snapshot(out, 2), 2u);
  EXPECT_EQ(out[0].seed, 3u);
  EXPECT_EQ(out[1].seed, 2u);

  // Simulate SIGKILL mid-Store of position 3: cell 1's sequence goes odd.
  auto* cell1_seq = reinterpret_cast<std::atomic<std::uint64_t>*>(
      region + Hist::CellBytes());
  cell1_seq->store(cell1_seq->load(std::memory_order_relaxed) + 1,
                   std::memory_order_release);

  // The latest published value survives a depth-N writer crash...
  Checksummed latest;
  ASSERT_EQ(reader_view.LoadLatestBounded(latest, 8),
            Hist::LoadResult::kValue);
  EXPECT_EQ(latest.seed, 3u);
  // ...and a snapshot is cut at the poisoned cell — tail only.
  ASSERT_EQ(reader_view.Snapshot(out, 2), 1u);
  EXPECT_EQ(out[0].seed, 3u);

  // The reclaiming sole writer retracts the in-flight cell and resumes.
  writer_view.RepairAfterWriterCrash();
  ASSERT_EQ(reader_view.Snapshot(out, 2), 1u);  // retracted, not resurrected
  writer_view.Store(Checksummed::Make(4));
  ASSERT_EQ(reader_view.Snapshot(out, 2), 2u);
  EXPECT_EQ(out[0].seed, 4u);
  EXPECT_EQ(out[1].seed, 3u);
  EXPECT_TRUE(out[0].Valid());
}

// Depth-1 parity for the same crash story: with one cell the in-flight cell
// IS the latest cell, so the bounded read reports the stall and the repair
// returns the buffer to "never written" — exactly MessageSlot's contract.
TEST(ConcurrencyMessageBuffer, DepthOneStalledWriterParityWithMessageSlot) {
  using Hist = MessageBuffer<Checksummed, 1, RegionStorage, EventCount>;
  alignas(64) unsigned char region[Hist::StorageBytes() + 64] = {};

  Hist writer_view(RegionStorage(region, sizeof(region), true));
  writer_view.Store(Checksummed::Make(7));

  Hist reader_view(RegionStorage(region, sizeof(region), false));
  Checksummed out;
  ASSERT_EQ(reader_view.LoadLatestBounded(out, 8), Hist::LoadResult::kValue);

  // Mid-Store crash: the (only) cell's sequence goes odd.
  auto* seq = reinterpret_cast<std::atomic<std::uint64_t>*>(region);
  seq->store(seq->load(std::memory_order_relaxed) + 1,
             std::memory_order_release);

  EXPECT_EQ(reader_view.LoadLatestBounded(out, 64),
            Hist::LoadResult::kStalled);

  writer_view.RepairAfterWriterCrash();
  EXPECT_EQ(reader_view.LoadLatestBounded(out, 8), Hist::LoadResult::kEmpty);
  EXPECT_FALSE(reader_view.LoadLatest(out));

  writer_view.Store(Checksummed::Make(8));
  ASSERT_EQ(reader_view.LoadLatestBounded(out, 8), Hist::LoadResult::kValue);
  EXPECT_EQ(out.seed, 8u);
  EXPECT_TRUE(out.Valid());
}

// The placement bet, depth-N edition: identical store sequences over heap
// and region placements yield byte-identical snapshots, and an attaching
// view (initialize == false) observes the live history it finds.
TEST(ConcurrencyMessageBuffer, RegionHostedRingEquivalentToHeap) {
  using HeapHist = MessageBuffer<Checksummed, 4, HeapStorage, EventCount>;
  using RegionHist = MessageBuffer<Checksummed, 4, RegionStorage, EventCount>;

  alignas(64) unsigned char region[RegionHist::StorageBytes() + 64] = {};
  HeapHist heap_hist;
  RegionHist region_hist(RegionStorage(region, sizeof(region), true));

  Checksummed h[4], r[4];
  EXPECT_EQ(heap_hist.Snapshot(h, 4), region_hist.Snapshot(r, 4));  // empty

  for (std::uint64_t seed = 1; seed <= 11; ++seed) {
    const Checksummed value = Checksummed::Make(seed);
    heap_hist.Store(value);
    region_hist.Store(value);
    const std::size_t hn = heap_hist.Snapshot(h, 4);
    const std::size_t rn = region_hist.Snapshot(r, 4);
    ASSERT_EQ(hn, rn);
    for (std::size_t i = 0; i < hn; ++i) {
      EXPECT_EQ(std::memcmp(&h[i], &r[i], sizeof(Checksummed)), 0);
    }
  }

  // Warm start: an attacher sees the history the initializer wrote.
  RegionHist attacher(RegionStorage(region, sizeof(region), false));
  Checksummed a[4];
  ASSERT_EQ(attacher.Snapshot(a, 4), 4u);
  EXPECT_EQ(a[0].seed, 11u);
  EXPECT_EQ(a[3].seed, 8u);
}

}  // namespace
