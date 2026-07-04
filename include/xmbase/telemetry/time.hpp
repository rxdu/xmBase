/*
 * telemetry/time.hpp
 *
 * The telemetry time base IS the family time base (xmbase/types/time.hpp):
 * one monotonic clock for every record, so a control glitch, a log line, and
 * a planning stall line up on one timeline by construction (design doc §5.1).
 * These aliases exist so instrumented code can be written entirely in the
 * xmotion::telemetry namespace; they are the same types, not parallel ones.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include "xmbase/types/time.hpp"

namespace xmotion {
namespace telemetry {

using Clock = ::xmotion::Clock;          // steady_clock — monotonic
using Timestamp = ::xmotion::Timestamp;  // ns-resolution monotonic time point
using Duration = ::xmotion::Duration;    // nanoseconds

// Current monotonic time. Cheap (vDSO clock_gettime on Linux); the S1
// contract is ONE clock read per record — stamp once, pass it along.
inline Timestamp Now() noexcept { return ::xmotion::Now(); }

}  // namespace telemetry
}  // namespace xmotion
