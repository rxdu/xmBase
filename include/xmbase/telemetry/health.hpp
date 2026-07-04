/*
 * telemetry/health.hpp
 *
 * Health is a CONVENTION over event+metric, not a fifth primitive (design doc
 * §5.2): a subsystem reports state TRANSITIONS (not steady state — see the
 * hysteresis criterion, scenario S3-A5); the SDK routes them as attributed
 * records. Unbound: non-Ok states go to stderr so a lib-only build is never
 * silent about a fault (ADR 0004 §2).
 *
 * `subsystem` and `detail` must be string literals or otherwise outlive the
 * call (they cross the seam as pointers; the SDK copies at the drain).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include "xmbase/telemetry/binding.hpp"
#include "xmbase/telemetry/context.hpp"
#include "xmbase/telemetry/time.hpp"

namespace xmotion {
namespace telemetry {

inline void ReportHealth(const char* subsystem, HealthState state,
                         const char* detail = "") noexcept {
  const Binding* b = ActiveBinding();
  if (b != nullptr) {
    b->report_health(subsystem, state, detail, CurrentContext(), Now());
  } else if (state != HealthState::kOk) {
    detail::UnboundReportHealth(subsystem, state, detail);
  }
}

}  // namespace telemetry
}  // namespace xmotion
