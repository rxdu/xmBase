/*
 * test_concurrency_lifecycle.cpp
 *
 * Construction/destruction churn for every concurrency type — the leak
 * surface the steady-state alloc gate deliberately does NOT cover (the
 * bench probe gates the hot path AFTER wiring; this gates the wiring).
 *
 * Runs meaningfully in every config; its teeth are the ASan+LSan and
 * valgrind legs: any storage handling that leaks, double-frees, or touches
 * freed cells during churn fails there. Region variants also verify the
 * no-ownership rule: destroying a placed view must not touch the region.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <cstdint>
#include <cstring>

#include "gtest/gtest.h"
#include "xmbase/concurrency/event_count.hpp"
#include "xmbase/concurrency/message_buffer.hpp"
#include "xmbase/concurrency/message_slot.hpp"
#include "xmbase/concurrency/spsc_queue.hpp"
#include "xmbase/concurrency/storage.hpp"

namespace {

using xmotion::concurrency::CondvarEventCount;
using xmotion::concurrency::EventCount;
using xmotion::concurrency::HeapStorage;
using xmotion::concurrency::MessageBuffer;
using xmotion::concurrency::MessageSlot;
using xmotion::concurrency::MutexMessageSlot;
using xmotion::concurrency::RegionStorage;
using xmotion::concurrency::SpscQueue;

struct Payload {
  std::uint64_t a, b, c;
};

constexpr int kChurn = 10000;

TEST(ConcurrencyLifecycle, HeapChurnAllTypes) {
  for (int i = 0; i < kChurn; ++i) {
    MessageSlot<Payload> slot;
    slot.Store({1, 2, 3});
    Payload p{};
    ASSERT_TRUE(slot.Load(p));

    MessageBuffer<Payload, 8> buffer;
    buffer.Store({4, 5, 6});
    ASSERT_TRUE(buffer.LoadLatest(p));

    SpscQueue<Payload> queue(16);
    ASSERT_TRUE(queue.TryPush({7, 8, 9}));
    ASSERT_TRUE(queue.TryPop(p));

    MutexMessageSlot<Payload> mslot;
    mslot.Store({1, 1, 1});
    ASSERT_TRUE(mslot.Load(p));

    EventCount ec;
    ec.NotifyAll();
    CondvarEventCount cec;
    cec.NotifyAll();
  }
}

TEST(ConcurrencyLifecycle, RegionChurnDoesNotTouchRegionOnDestroy) {
  using RegionBuffer = MessageBuffer<Payload, 8, RegionStorage>;
  alignas(64) static unsigned char region[RegionBuffer::StorageBytes()];

  // Initialize once, store a sentinel, destroy the view.
  {
    RegionBuffer init(RegionStorage(region, sizeof(region), true));
    init.Store({11, 22, 33});
  }
  unsigned char before[sizeof(region)];
  std::memcpy(before, region, sizeof(region));

  // Churn attaching views: construction/destruction of a placed view must
  // be a pure view operation — the region bytes never change.
  for (int i = 0; i < kChurn; ++i) {
    RegionBuffer view(RegionStorage(region, sizeof(region), false));
    Payload p{};
    ASSERT_TRUE(view.LoadLatest(p));
    ASSERT_EQ(p.a, 11u);
  }
  ASSERT_EQ(std::memcmp(before, region, sizeof(region)), 0)
      << "attach/destroy churn mutated the region";
}

}  // namespace
