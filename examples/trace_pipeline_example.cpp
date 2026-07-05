/*
 * trace_pipeline_example.cpp
 *
 * REFERENCE USAGE — the canonical trace-propagation pattern from
 * docs/telemetry/guide.md: mint a trace at ingress, scope each stage,
 * carry identity across a thread boundary in a plain message envelope
 * (Inject/Extract), and LINK fan-in inputs to the consuming span.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <array>
#include <chrono>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>

#include "xmbase/telemetry/telemetry.hpp"

namespace tel = xmotion::telemetry;

namespace {

// A plain envelope — no ROS, no DDS: correlation identity rides as bytes.
struct Envelope {
  std::array<std::uint8_t, tel::kContextWireSize> ctx;
  int kind;  // 0=pose 1=map 2=goal
};

class Queue {
 public:
  void Push(Envelope e) { std::lock_guard<std::mutex> lk(m_); q_.push(e); }
  std::optional<Envelope> Pop() {
    std::lock_guard<std::mutex> lk(m_);
    if (q_.empty()) return std::nullopt;
    Envelope e = q_.front(); q_.pop();
    return e;
  }

 private:
  std::mutex m_;
  std::queue<Envelope> q_;
};

// Producer thread: works UNDER the iteration's trace, ships its context.
void ProduceInput(tel::Context root, int kind, const char* name, Queue& out) {
  tel::ContextGuard g(root);              // set + auto-restore (never bare Set)
  XM_SPAN(name);                          // e.g. "demo.input.pose" — child span
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  out.Push(Envelope{tel::Inject(tel::CurrentContext()), kind});
}

}  // namespace

int main() {
  XM_INFO("trace pipeline demo: 3 producer threads -> gather -> plan -> output");

  const tel::Context root = tel::NewTrace();  // ONE trace per unit of work
  Queue q;
  std::thread pose([&] { ProduceInput(root, 0, "demo.input.pose", q); });
  std::thread map([&] { ProduceInput(root, 1, "demo.input.map", q); });
  std::thread goal([&] { ProduceInput(root, 2, "demo.input.goal", q); });

  {
    tel::ContextGuard g(root);
    XM_SPAN("demo.plan.iteration");

    {
      // Fan-in: LINK each consumed input's context (association, not parent).
      tel::Span gather("demo.plan.gather_inputs");
      int got = 0;
      while (got < 3) {
        if (auto e = q.Pop()) {
          const tel::Context in = tel::Extract(e->ctx.data(), e->ctx.size());
          if (in.valid()) gather.AddLink(in);  // validate at the boundary!
          ++got;
        } else {
          std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
      }
    }
    {
      XM_SPAN_LINKED("demo.plan.search", root);  // single-link shorthand form
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      XM_INFO("search done under trace {:x}{:x}",
              tel::CurrentContext().trace.hi, tel::CurrentContext().trace.lo);
    }
  }

  pose.join(); map.join(); goal.join();
  XM_INFO("done — with the SDK bound, every stage above lands on ONE timeline "
          "under one TraceId, with the gather span linking all 3 inputs");
  return 0;
}
