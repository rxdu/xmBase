/*
 * matrix_utils.hpp
 *
 * Small Eigen helpers shared across the family (estimation, kinematics,
 * attitude math).
 *
 * Copyright (c) 2024-2026 Ruixiang Du (rdu)
 */

#ifndef XMBASE_MATH_MATRIX_UTILS_HPP
#define XMBASE_MATH_MATRIX_UTILS_HPP

#include <eigen3/Eigen/Dense>

namespace xmotion {
namespace MathUtils {
inline Eigen::Matrix<double, 3, 3> SkewSymmetric(const Eigen::Vector3d& v) {
  Eigen::Matrix<double, 3, 3> m;
  // clang-format off
  m <<     0, -v.z(),  v.y(),
       v.z(),      0, -v.x(),
      -v.y(),  v.x(),      0;
  // clang-format on
  return m;
}
}  // namespace MathUtils
}  // namespace xmotion

#endif  // XMBASE_MATH_MATRIX_UTILS_HPP
