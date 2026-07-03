/*
 * vector.hpp
 *
 * Plain-old-data vector structs for the driver / wire layer, where pulling in
 * Eigen is undesirable (small sensor payloads, ABI-stable buffers). For math in
 * the motion/control layer use the Eigen-backed aliases in geometry.hpp instead.
 *
 * Members are default-initialized to zero: a default-constructed vector is a
 * well-defined zero vector, never garbage (these back fields like ImuData::accel).
 *
 * Copyright (c) 2021-2026 Ruixiang Du (rdu)
 */

#pragma once

namespace xmotion {

template <typename T>
struct vector3_t {
  T x{};
  T y{};
  T z{};
};

using Vector3f = vector3_t<float>;
using Vector3d = vector3_t<double>;

template <typename T>
struct vector4_t {
  T x{};
  T y{};
  T z{};
  T w{};
};

using Vector4f = vector4_t<float>;
using Vector4d = vector4_t<double>;

}  // namespace xmotion
