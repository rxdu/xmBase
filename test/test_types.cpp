/*
 * test_types.cpp — properties of the GEOMETRY tier of the type vocabulary
 * (Eigen-backed types + opt-in quantities). Links xmBaseGeometry; the
 * Eigen-free wire tier is covered by test_types_core.cpp, which links only
 * the core target (the 0.5.0 target split).
 *
 * Focus on the things the restructure is meant to guarantee:
 *   - default-construction is well-defined (no garbage poses/vectors),
 *   - the legacy include paths and names still resolve (μ/∇ source compat),
 *   - opt-in quantities are distinct types with closed arithmetic.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <type_traits>

#include "gtest/gtest.h"

// Legacy facade include paths must keep working (this is what xmDriver / xmNavigation use).
#include "xmbase/types/base_types.hpp"
#include "xmbase/types/geometry_types.hpp"
// New umbrella + opt-in strong types.
#include "xmbase/types/types.hpp"
#include "xmbase/types/quantities.hpp"

namespace {

// --- default-initialization (the bug class this restructure removes) --------

TEST(TypesTest, PoseDefaultsToIdentityNotGarbage) {
  xmotion::Pose p;  // default-constructed
  EXPECT_TRUE(p.position.isZero());
  // Eigen leaves quaternions uninitialized by default; ours must be identity.
  EXPECT_DOUBLE_EQ(p.orientation.w(), 1.0);
  EXPECT_DOUBLE_EQ(p.orientation.x(), 0.0);
  EXPECT_DOUBLE_EQ(p.orientation.y(), 0.0);
  EXPECT_DOUBLE_EQ(p.orientation.z(), 0.0);
}

TEST(TypesTest, TwistAndWrenchDefaultToZero) {
  xmotion::Twist t;
  EXPECT_TRUE(t.linear.isZero());
  EXPECT_TRUE(t.angular.isZero());
  xmotion::Wrench w;
  EXPECT_TRUE(w.force.isZero());
  EXPECT_TRUE(w.torque.isZero());
}

TEST(TypesTest, OdometryDefaultIsWellFormed) {
  xmotion::Odometry o;  // mirrors `Odometry odom_;` in xmNavigation
  EXPECT_TRUE(o.pose.position.isZero());
  EXPECT_DOUBLE_EQ(o.pose.orientation.w(), 1.0);
  EXPECT_TRUE(o.twist.linear.isZero());
  EXPECT_TRUE(o.child_frame_id.empty());
}

// Existing aggregate-init usage in xmNavigation must still compile/behave:
//   odom_.pose  = {{0,0,0}, {1,0,0,0}};
//   odom_.twist = {{0,0,0}, {0,0,0}};
TEST(TypesTest, AggregateInitStillWorks) {
  xmotion::Pose p = {{1, 2, 3}, {1, 0, 0, 0}};
  EXPECT_DOUBLE_EQ(p.position.x(), 1.0);
  EXPECT_DOUBLE_EQ(p.orientation.w(), 1.0);
  xmotion::Twist t = {{0, 0, 0}, {0, 0, 0}};
  EXPECT_TRUE(t.linear.isZero());
}

// --- opt-in strong quantities ----------------------------------------------

TEST(TypesTest, QuantitiesAreDistinctTypes) {
  // The whole point: Force and LinearVelocity are not interchangeable.
  static_assert(!std::is_same<xmotion::Force, xmotion::LinearVelocity>::value,
                "strong quantities must be distinct types");
  static_assert(!std::is_convertible<xmotion::Force, xmotion::Torque>::value,
                "strong quantities must not implicitly convert across tags");
}

TEST(TypesTest, QuantityArithmeticIsClosedAndCorrect) {
  xmotion::Force a(1, 2, 3);
  xmotion::Force b(4, 5, 6);
  xmotion::Force c = a + b;
  EXPECT_DOUBLE_EQ(c.x(), 5.0);
  EXPECT_DOUBLE_EQ(c.y(), 7.0);
  EXPECT_DOUBLE_EQ((2.0 * a).y(), 4.0);
  EXPECT_TRUE((a - a) == xmotion::Force::Zero());
  // Escape hatch reaches Eigen for real math.
  EXPECT_DOUBLE_EQ(c.vec().norm(), Eigen::Vector3d(5, 7, 9).norm());
}

}  // namespace
