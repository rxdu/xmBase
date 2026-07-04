/*
 * mpsc_rt_logger_example.cpp
 *
 * Multi-producer hard-real-time logging (MpscRtLogger): several RT threads —
 * e.g. independent control loops or sensor callbacks — share ONE logger and log
 * concurrently. The hot path is lock-free, allocation-free, syscall-free, and
 * drops (never blocks) when the ring is full; a single drain thread does the I/O.
 *
 * Use RtLogger (SPSC, wait-free) when there is exactly one producer; use this
 * when there is more than one. This is an example, NOT a unit test.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "xmbase/logging/rt_logger_mpsc.hpp"
#include "xmbase/telemetry/telemetry.hpp"

int main() {
  XM_INFO("mpsc_rt_logger_example: 3 control loops sharing one RT logger");

  // One logger, shared by every producer thread. Console sink; producers never
  // touch the sink — the drain thread does.
  xmotion::MpscRtLogger rt("multi_loop", /*ring_capacity=*/4096);

  constexpr int kLoops = 3;
  constexpr int kCyclesPerLoop = 1000;
  const auto period = std::chrono::microseconds(200);  // ~5 kHz per loop

  std::vector<std::thread> loops;
  for (int loop_id = 0; loop_id < kLoops; ++loop_id) {
    loops.emplace_back([&rt, loop_id, period] {
      for (int cycle = 0; cycle < kCyclesPerLoop; ++cycle) {
        // Wait-free-ish hot-path log from this RT thread; safe to call
        // concurrently with the other loops.
        XLOG_RT_INFO(rt, "loop={} cycle={} u={:.3f}", loop_id, cycle,
                     cycle * 0.001);
        std::this_thread::sleep_for(period);
      }
    });
  }
  for (auto& t : loops) t.join();

  rt.Flush();  // NON-RT: drain the ring before reporting/exit
  XM_INFO("mpsc_rt_logger_example: done, {} records dropped (ring-full)",
            rt.dropped());
  return 0;
}
