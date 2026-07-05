/*
 * test_event.cpp
 *
 * Tests for the in-process event system (dispatcher/emitter, async
 * variant, thread-safe queue).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "xmbase/container/thread_safe_queue.hpp"
#include "xmbase/event/async_event_dispatcher.hpp"
#include "xmbase/event/async_event_emitter.hpp"
#include "xmbase/event/event.hpp"
#include "xmbase/event/event_dispatcher.hpp"
#include "xmbase/event/event_emitter.hpp"

using namespace xmotion;

TEST(ThreadSafeQueueTest, PushPop) {
  ThreadSafeQueue<int> q;
  q.Push(1);
  q.Push(2);
  EXPECT_EQ(q.Pop(), 1);
  int v = 0;
  EXPECT_TRUE(q.TryPop(v));
  EXPECT_EQ(v, 2);
  EXPECT_FALSE(q.TryPop(v));
}

TEST(ThreadSafeQueueTest, BoundedOverflowDropsOldest) {
  ThreadSafeQueue<int, 4> q;
  for (int i = 0; i < 6; ++i) q.Push(i);
  EXPECT_EQ(q.Pop(), 2);  // 0 and 1 were dropped
}

TEST(ThreadSafeQueueTest, MoveTransfersContents) {
  ThreadSafeQueue<int> a;
  a.Push(7);
  ThreadSafeQueue<int> b(std::move(a));
  int v = 0;
  EXPECT_TRUE(b.TryPop(v));
  EXPECT_EQ(v, 7);
}

TEST(EventTest, SyncDispatch) {
  int got_a = 0;
  double got_b = 0;
  std::string got_c;
  EventDispatcher::GetInstance().RegisterHandler(
      "sync_test_event", [&](std::shared_ptr<BaseEvent> event) {
        auto data =
            std::static_pointer_cast<Event<int, double, std::string>>(event)
                ->GetData();
        got_a = std::get<0>(data);
        got_b = std::get<1>(data);
        got_c = std::get<2>(data);
      });

  EventEmitter emitter;
  emitter.Emit<Event<int, double, std::string>>(
      EventSource::kApplication, "sync_test_event", 42, 3.14, "hello");

  EXPECT_EQ(got_a, 42);
  EXPECT_DOUBLE_EQ(got_b, 3.14);
  EXPECT_EQ(got_c, "hello");
}

TEST(EventTest, AsyncDispatchHandlesQueuedEvents) {
  std::vector<int> received;
  AsyncEventDispatcher::GetInstance().RegisterHandler(
      "async_test_event", [&](std::shared_ptr<BaseEvent> event) {
        received.push_back(
            std::get<0>(std::static_pointer_cast<Event<int>>(event)->GetData()));
      });

  AsyncEventEmitter emitter;
  for (int i = 0; i < 5; ++i) {
    emitter.Emit<Event<int>>(EventSource::kApplication, "async_test_event", i);
  }

  // consume from a dedicated handler thread (dispatch and handling must
  // stay on distinct threads by contract)
  std::thread handler([] { AsyncEventDispatcher::GetInstance().HandleEvents(); });
  handler.join();

  ASSERT_EQ(received.size(), 5u);
  for (int i = 0; i < 5; ++i) EXPECT_EQ(received[i], i);
}
