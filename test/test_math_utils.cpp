/*
 * test_math_utils.cpp
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <gtest/gtest.h>

#include "xmbase/math/matrix_utils.hpp"

using namespace xmotion;

TEST(MathUtilsTest, SkewSymmetricAntisymmetry) {
  Eigen::Vector3d v(1.0, -2.0, 3.5);
  auto m = MathUtils::SkewSymmetric(v);
  EXPECT_TRUE(m.transpose().isApprox(-m));
  EXPECT_DOUBLE_EQ(m.trace(), 0.0);
}

TEST(MathUtilsTest, SkewSymmetricMatchesCrossProduct) {
  Eigen::Vector3d v(0.3, 1.2, -0.7);
  Eigen::Vector3d w(-2.0, 0.5, 4.0);
  EXPECT_TRUE((MathUtils::SkewSymmetric(v) * w).isApprox(v.cross(w)));
}
