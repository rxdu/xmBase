/*
 * rt_logger_example.cpp
 *
 * Demonstrates dual-mode logging: the soft-RT XLOG_* macros for setup/teardown
 * (async — formats in the caller, a worker thread does the I/O), and the
 * hard-RT XLOG_RT_* macros inside a tight loop (wait-free, no heap, no syscall,
 * drop-on-full).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <chrono>
#include <thread>

#include "xmsigma/logging/rt_logger.hpp"
#include "xmsigma/logging/xlogger.hpp"

int main() {
  // Soft-RT: fine for non-deadline code (startup banner, summaries).
  XLOG_INFO("rt_logger_example: starting a ~10 kHz control-loop demo");

  // Hard-RT: one logger owned by the loop. Console sink; the producer never
  // touches the sink — a drain thread does the I/O.
  xmotion::RtLogger rt("ctrl_loop", /*ring_capacity=*/4096);

  const auto period = std::chrono::microseconds(100);  // ~10 kHz
  for (int cycle = 0; cycle < 2000; ++cycle) {
    const double tau = cycle * 0.01;

    // Wait-free hot-path log: formats into a fixed ring slot and publishes.
    XLOG_RT_INFO(rt, "cycle {} tau={:.3f}", cycle, tau);

    std::this_thread::sleep_for(period);
  }

  rt.Flush();  // NON-RT: drain the ring before we report/exit
  XLOG_INFO("rt_logger_example: done, {} records dropped (ring-full)",
            rt.dropped());
  return 0;
}
