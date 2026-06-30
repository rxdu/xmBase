/*
 * xlogger_example.cpp
 *
 * Soft-real-time logging (XLOG_*): fmt-style and stream-style messages, runtime
 * and compile-time level control, and thread-safe use. This is an example, NOT a
 * unit test (no assertions); see ../test/test_xlogger.cpp.
 *
 * Try it with different levels:
 *   XLOG_LEVEL=0 ./xlogger_example            # show everything (trace+)
 *   XLOG_LEVEL=4 ./xlogger_example            # errors and worse only
 *   XLOG_ENABLE_LOGFILE=1 ./xlogger_example   # also write to ~/.xmotion/log
 *
 * Copyright (c) 2024-2026 Ruixiang Du (rdu)
 */

#include <thread>
#include <vector>

#include "xmsigma/logging/xlogger.hpp"

int main() {
  // fmt-style: format strings use {} placeholders, NOT printf %d/%f.
  const int rpm = 3200;
  const double current = 4.75;
  XLOG_INFO("motor spun up: {} RPM at {:.2f} A", rpm, current);
  XLOG_WARN("temperature {} C approaching limit", 71);
  XLOG_ERROR("encoder {} dropped {} ticks", 2, 14);

  // Stream-style: handy for types with operator<< but no fmt formatter. The
  // argument expression is only evaluated if the level is enabled.
  XLOG_DEBUG_STREAM("debug detail: " << "phase=" << 'A' << " count=" << 42);

  // Runtime level control. The initial level comes from $XLOG_LEVEL (default
  // INFO); override it at runtime here.
  XLOG_INFO("current level = {}", static_cast<int>(XLOG_GET_LEVEL()));
  XLOG_LEVEL(static_cast<int>(xmotion::LogLevel::kWarn));
  XLOG_INFO("this INFO is now suppressed (level raised to WARN)");
  XLOG_WARN("this WARN still shows");
  XLOG_LEVEL(static_cast<int>(xmotion::LogLevel::kInfo));  // restore

  // Compile-time floor: building with -DXMSIGMA_ACTIVE_LEVEL=2 strips TRACE/DEBUG
  // sites entirely (zero cost), independent of the runtime level above.
  XLOG_TRACE("trace sites compile out when XMSIGMA_ACTIVE_LEVEL > 0");

  // The logger is thread-safe: many threads may log concurrently.
  std::vector<std::thread> workers;
  for (int id = 0; id < 4; ++id) {
    workers.emplace_back([id] {
      for (int i = 0; i < 3; ++i) XLOG_INFO("worker {} tick {}", id, i);
    });
  }
  for (auto& t : workers) t.join();

  return 0;
}
