/*
 * concurrency_worker_inbox_example.cpp
 *
 * The bounded worker inbox: commands flow from a producer thread to a
 * worker that parks when idle — the job the deprecated event/ dispatchers
 * did (AsyncEventDispatcher = thread_safe_queue + condvar + worker loop),
 * rebuilt on the concurrency primitives in ~30 lines of application code.
 *
 * What changes vs the deprecated pair, deliberately:
 *   - BOUNDED, not unbounded: thread_safe_queue grew without limit — an
 *     R7 violation (nothing may grow unbounded on a robot). Here capacity
 *     is declared at wiring time and overflow is an EXPLICIT, counted
 *     decision at the producer (reject, coalesce, or shed — caller policy).
 *   - Parking without lost wakeups: EventCount is an eventcount — the
 *     producer's NotifyAll after a push can never slip between the worker's
 *     emptiness check and its sleep (see event_count.hpp for the protocol; it is
 *     also why timed condvars are avoided under TSan on GCC 11).
 *   - SPSC by contract: one producer, one worker. For N producers, give
 *     each its own queue and let the worker sweep them (the fan-in pattern
 *     xmMessaging uses per-subscriber) — do NOT share one SpscQueue.
 *
 * If you need consumption semantics, handler priorities, or GUI-thread
 * marshaling, that is a UI event system — quickviz core/event owns that
 * plane (ADR 0007 §2); it never belonged in the foundation.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "xmbase/concurrency/spsc_queue.hpp"
#include "xmbase/concurrency/event_count.hpp"

namespace {

struct Command {
  std::uint32_t kind;
  std::uint64_t arg;
};

}  // namespace

int main() {
  using namespace std::chrono_literals;

  // Wiring time: capacity is declared, memory is fixed from here on (R7).
  xmotion::concurrency::SpscQueue<Command> inbox(64);
  xmotion::concurrency::EventCount waiter;
  std::atomic<bool> shutdown{false};

  // Worker: drain what's there, then park until notified (bounded park —
  // the loop re-checks its own shutdown flag at least every max_park, so
  // no external lifecycle framework is needed).
  std::thread worker([&] {
    std::uint64_t handled = 0;
    Command cmd{};
    for (;;) {
      while (inbox.TryPop(cmd)) {
        ++handled;  // ... do the work ...
      }
      if (shutdown.load(std::memory_order_acquire)) break;
      waiter.WaitFor(50ms, [&] {
        return inbox.Size() != 0 || shutdown.load(std::memory_order_acquire);
      });
    }
    std::printf("worker: handled %lu commands\n",
                static_cast<unsigned long>(handled));
  });

  // Producer: overflow is explicit and counted — never silent, never
  // blocking. (Policy here: reject and count. Coalescing or shedding are
  // equally valid caller choices; the point is the caller DECIDES.)
  std::uint64_t sent = 0, rejected = 0;
  for (std::uint64_t i = 0; i < 100000; ++i) {
    if (inbox.TryPush(Command{1, i})) {
      ++sent;
      waiter.NotifyAll();  // transition site: work became pending
    } else {
      ++rejected;
    }
  }

  shutdown.store(true, std::memory_order_release);
  waiter.NotifyAll();
  worker.join();

  std::printf("producer: sent %lu, rejected %lu (sent+rejected==100000: %s)\n",
              static_cast<unsigned long>(sent),
              static_cast<unsigned long>(rejected),
              (sent + rejected == 100000) ? "yes" : "NO — bug");
  return (sent + rejected == 100000) ? 0 : 1;
}
