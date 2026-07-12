/*
 * test_concurrency_placement.cpp — the placement-parameterization bet as a
 * permanent test (ADR 0007 W1): the SAME algorithm over HeapStorage and
 * RegionStorage must behave identically, and a region's layout must be a
 * pure function of the placed types (never of who attached first).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <cstdint>
#include <cstring>

#include "gtest/gtest.h"
#include "xmbase/concurrency/spsc_queue.hpp"
#include "xmbase/concurrency/message_buffer.hpp"
#include "xmbase/concurrency/storage.hpp"

namespace {

using xmotion::concurrency::SpscQueue;
using xmotion::concurrency::EventCount;
using xmotion::concurrency::HeapStorage;
using xmotion::concurrency::MessageBuffer;
using xmotion::concurrency::RegionStorage;

// Padding-free by construction: the equivalence checks compare raw bytes,
// so the type must not carry indeterminate padding.
struct Value {
  std::uint64_t a = 0;
  std::uint64_t b = 0;
  std::uint64_t c = 0;

  static Value Make(std::uint64_t seed) {
    return Value{seed, seed * 31u, seed ^ 0xABu};
  }
};
static_assert(sizeof(Value) == 3 * sizeof(std::uint64_t),
              "Value must be padding-free for byte comparisons");

// Identical Store/Load sequences on both placements yield byte-identical
// results — the seqlock algorithm is placement-blind.
TEST(ConcurrencyPlacement, MessageBufferEquivalentOnHeapAndRegion) {
  using HeapSlot = MessageBuffer<Value, HeapStorage, EventCount>;
  using RegionSlot = MessageBuffer<Value, RegionStorage, EventCount>;

  alignas(64) unsigned char region[RegionSlot::StorageBytes() + 64] = {};
  HeapSlot heap_slot;
  RegionSlot region_slot(RegionStorage(region, sizeof(region), true));

  Value h, r;
  EXPECT_EQ(heap_slot.Load(h), region_slot.Load(r));  // both empty

  for (std::uint64_t seed = 1; seed <= 64; ++seed) {
    const Value value = Value::Make(seed);
    heap_slot.Store(value);
    region_slot.Store(value);
    ASSERT_TRUE(heap_slot.Load(h));
    ASSERT_TRUE(region_slot.Load(r));
    EXPECT_EQ(std::memcmp(&h, &r, sizeof(Value)), 0);
  }
}

// Identical push/pop sequences on both placements yield identical
// accept/refuse decisions and identical values.
TEST(ConcurrencyPlacement, SpscQueueEquivalentOnHeapAndRegion) {
  using HeapQueue = SpscQueue<Value, HeapStorage>;
  using RegionQueue = SpscQueue<Value, RegionStorage>;
  constexpr std::uint32_t kDepth = 4;

  alignas(64) unsigned char region[RegionQueue::ControlBytes() +
                                   kDepth * RegionQueue::RecordBytes() +
                                   192] = {};
  HeapQueue heap_queue(kDepth);
  RegionQueue region_queue(kDepth,
                           RegionStorage(region, sizeof(region), true));

  // Interleaved pushes/pops, driven past full and back to empty.
  for (std::uint64_t seed = 1; seed <= 6; ++seed) {
    EXPECT_EQ(heap_queue.TryPush(Value::Make(seed)),
              region_queue.TryPush(Value::Make(seed)));
    EXPECT_EQ(heap_queue.Size(), region_queue.Size());
    EXPECT_EQ(heap_queue.Full(), region_queue.Full());
  }
  Value h, r;
  for (;;) {
    const bool heap_popped = heap_queue.TryPop(h);
    const bool region_popped = region_queue.TryPop(r);
    ASSERT_EQ(heap_popped, region_popped);
    if (!heap_popped) {
      break;
    }
    EXPECT_EQ(std::memcmp(&h, &r, sizeof(Value)), 0);
  }
}

// Layout determinism: two placements over the same region, fed the same
// carve sequence, consume identical offsets — the contract that lets every
// attacher of a shared region derive the same layout independently.
TEST(ConcurrencyPlacement, RegionLayoutIsDeterministic) {
  alignas(64) unsigned char region[4096] = {};

  RegionStorage first(region, sizeof(region), true);
  auto* s1 = first.MakeSingle<std::uint64_t>();
  auto* a1 = first.MakeArray<Value>(8);
  const std::size_t used_first = first.used();

  RegionStorage second(region, sizeof(region), false);  // attach: no re-zero
  auto* s2 = second.MakeSingle<std::uint64_t>();
  auto* a2 = second.MakeArray<Value>(8);

  EXPECT_EQ(s1, s2);
  EXPECT_EQ(a1, a2);
  EXPECT_EQ(used_first, second.used());
  // Cells are cache-line aligned.
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(a1) % 64, 0u);
}

// Warm start: an attaching view (initialize == false) must observe the data
// the initializing view wrote — attaching never re-zeros.
TEST(ConcurrencyPlacement, AttachingViewPreservesLiveData) {
  using Slot = MessageBuffer<Value, RegionStorage, EventCount>;
  alignas(64) unsigned char region[Slot::StorageBytes() + 64] = {};

  Slot creator(RegionStorage(region, sizeof(region), true));
  creator.Store(Value::Make(99));

  Slot attacher(RegionStorage(region, sizeof(region), false));
  Value out;
  ASSERT_TRUE(attacher.Load(out));  // the warm-start value survived
  EXPECT_EQ(out.a, 99u);
  EXPECT_EQ(out.b, 99u * 31u);
}

}  // namespace
