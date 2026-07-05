/*
 * xm_logging_example.cpp
 *
 * Unified logging (XM_* — the telemetry event() verb): fmt-style and stream-style messages, runtime
 * and compile-time level control, and thread-safe use. This is an example, NOT a
 * unit test (no assertions); see ../test/test_logging.cpp.
 *
 * Try it with different levels:
 *   XM_LOG_LEVEL=0 ./xm_logging_example            # show everything (trace+)
 *   XM_LOG_LEVEL=4 ./xm_logging_example            # errors and worse only
 *
 * Copyright (c) 2024-2026 Ruixiang Du (rdu)
 */

#include <thread>
#include <vector>

#include "xmbase/telemetry/telemetry.hpp"

int main() {
  // fmt-style: format strings use {} placeholders, NOT printf %d/%f.
  const int rpm = 3200;
  const double current = 4.75;
  XM_INFO("motor spun up: {} RPM at {:.2f} A", rpm, current);
  XM_WARN("temperature {} C approaching limit", 71);
  XM_ERROR("encoder {} dropped {} ticks", 2, 14);

  // Stream-style: handy for types with operator<< but no fmt formatter. The
  // argument expression is only evaluated if the level is enabled.
  XM_DEBUG_STREAM("debug detail: " << "phase=" << 'A' << " count=" << 42);

  // Runtime level control. The initial level comes from $XM_LOG_LEVEL (default
  // INFO); override it at runtime here.
  XM_INFO("current level = {}", static_cast<int>(xmotion::telemetry::GetLogLevel()));
  xmotion::telemetry::SetLogLevel(xmotion::telemetry::Severity::kWarn);
  XM_INFO("this INFO is now suppressed (level raised to WARN)");
  XM_WARN("this WARN still shows");
  xmotion::telemetry::SetLogLevel(xmotion::telemetry::Severity::kInfo);  // restore

  // Compile-time floor: building with -DXM_TELEMETRY_LEVEL=2 strips TRACE/DEBUG
  // sites entirely (zero cost), independent of the runtime level above.
  XM_TRACE("trace sites compile out when XM_TELEMETRY_LEVEL > 0");

  // The logger is thread-safe: many threads may log concurrently.
  std::vector<std::thread> workers;
  for (int id = 0; id < 4; ++id) {
    workers.emplace_back([id] {
      for (int i = 0; i < 3; ++i) XM_INFO("worker {} tick {}", id, i);
    });
  }
  for (auto& t : workers) t.join();

  return 0;
}
