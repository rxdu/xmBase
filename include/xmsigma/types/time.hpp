/*
 * time.hpp
 *
 * One time vocabulary for the whole family. Before this, timestamps were spelled
 * three different ways (a raw uint32_t in ImuData, a double in TrajectoryPoint,
 * and the unused RSClock aliases here); pick ONE and document its meaning.
 *
 * Convention:
 *   - Clock is steady_clock — monotonic, never steps backward. Use it for
 *     durations, deadlines, and loop timing (NOT for human-readable wall time;
 *     the logging sink uses system_clock for that, see logging/).
 *   - Timestamp is a steady-clock time_point at nanosecond resolution.
 *   - Duration is nanoseconds.
 *
 * Copyright (c) 2021-2026 Ruixiang Du (rdu)
 */

#pragma once

#include <chrono>

namespace xmotion {

// Monotonic clock for all in-process timing.
using Clock = std::chrono::steady_clock;

// Nanosecond-resolution monotonic time point and duration.
using Timestamp = std::chrono::time_point<Clock, std::chrono::nanoseconds>;
using Duration = std::chrono::nanoseconds;

// Current monotonic time, normalized to the family Timestamp resolution.
inline Timestamp Now() {
  return std::chrono::time_point_cast<std::chrono::nanoseconds>(Clock::now());
}

// Deprecated legacy spellings — kept for source compatibility. Prefer
// Clock / Timestamp in new code.
using RSClock = Clock;
using RSTimePoint = std::chrono::time_point<RSClock>;

}  // namespace xmotion
