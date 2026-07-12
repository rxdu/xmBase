/*
 * @file async_event_dispatcher.hpp
 * @date 10/8/24
 * @brief
 *
 * DEPRECATED-FOR-REMOVAL at xmBase 0.6.0 (ADR 0007): xmbase/event/ retires
 * together with container/thread_safe_queue (its only consumer is xmBase's
 * own async dispatcher). Successors by plane: UI/command events belong to
 * the viz library's GUI event system; observability/robot data belongs to
 * xmMessaging topics. New code must not add includes of this header.
 *
 * @copyright Copyright (c) 2024 Ruixiang Du (rdu)
 */

#ifndef XMBASE_EVENT_ASYNC_EVENT_DISPATCHER_HPP
#define XMBASE_EVENT_ASYNC_EVENT_DISPATCHER_HPP

#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <functional>
#include <string>
#include <queue>
#include <atomic>

#include "xmbase/event/event.hpp"
#include "xmbase/container/thread_safe_queue.hpp"

namespace xmotion {
class AsyncEventDispatcher {
 public:
  using HandlerFunc = std::function<void(std::shared_ptr<BaseEvent>)>;

  // public interface
  static AsyncEventDispatcher& GetInstance();
  void RegisterHandler(const std::string& event_name, HandlerFunc handler);
  void Dispatch(std::shared_ptr<BaseEvent> event);
  void HandleEvents();

 private:
  AsyncEventDispatcher() = default;

  std::mutex handler_mutex_;
  std::unordered_map<std::string, std::vector<HandlerFunc>> handlers_;
  ThreadSafeQueue<std::shared_ptr<BaseEvent>> event_queue_;

  std::atomic<std::thread::id> dispatch_thread_id_{};
  std::atomic<std::thread::id> handle_events_thread_id_{};
};
}  // namespace xmotion

#endif  // XMBASE_EVENT_ASYNC_EVENT_DISPATCHER_HPP