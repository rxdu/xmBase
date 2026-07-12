/*
 * @file event_async_emitter.hpp
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
#ifndef XMBASE_EVENT_ASYNC_EVENT_EMITTER_HPP
#define XMBASE_EVENT_ASYNC_EVENT_EMITTER_HPP

#include "xmbase/event/async_event_dispatcher.hpp"

namespace xmotion {
class AsyncEventEmitter {
 public:
  template <typename EventT, typename... Args>
  void Emit(EventSource type, const std::string& event_name, Args... args) {
    auto event = std::make_shared<EventT>(type, event_name, args...);
    AsyncEventDispatcher::GetInstance().Dispatch(event);
  }
};
}  // namespace xmotion

#endif  // XMBASE_EVENT_ASYNC_EVENT_EMITTER_HPP
