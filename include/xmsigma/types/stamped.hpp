/*
 * stamped.hpp
 *
 * Stamped<T> — pair any value with the monotonic time it was sampled/produced.
 * Addresses the recurring "value and its timestamp travel separately" pattern
 * (a sample, a measurement, an estimate) with one small, explicit type.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include <utility>

#include "xmsigma/types/time.hpp"

namespace xmotion {

template <typename T>
struct Stamped {
  Timestamp stamp{};
  T value{};

  Stamped() = default;
  Stamped(Timestamp t, T v) : stamp(t), value(std::move(v)) {}
};

// Convenience: stamp a value with the current monotonic time.
template <typename T>
Stamped<T> StampNow(T value) {
  return Stamped<T>(Now(), std::move(value));
}

}  // namespace xmotion
