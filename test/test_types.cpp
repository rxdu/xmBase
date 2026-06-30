/*
 * test_types.cpp — properties of the common type vocabulary.
 *
 * Focus on the things the restructure is meant to guarantee:
 *   - default-construction is well-defined (no garbage poses/vectors),
 *   - the legacy include paths and names still resolve (μ/∇ source compat),
 *   - the time vocabulary is monotonic,
 *   - Stamped<T> pairs a value with a timestamp,
 *   - opt-in quantities are distinct types with closed arithmetic.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <type_traits>

#include "gtest/gtest.h"

// Legacy facade include paths must keep working (this is what xmMu / xmNabla use).
#include "xmsigma/types/base_types.hpp"
#include "xmsigma/types/geometry_types.hpp"
// New umbrella + opt-in strong types.
#include "xmsigma/types/types.hpp"
#include "xmsigma/types/quantities.hpp"

namespace {

// --- default-initialization (the bug class this restructure removes) --------

TEST(TypesTest, PodVectorsDefaultToZero) {
  xmotion::Vector3f v3{};
  EXPECT_FLOAT_EQ(v3.x, 0.0f);
  EXPECT_FLOAT_EQ(v3.y, 0.0f);
  EXPECT_FLOAT_EQ(v3.z, 0.0f);

  xmotion::Vector4d v4{};
  EXPECT_DOUBLE_EQ(v4.w, 0.0);
}

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
  xmotion::Odometry o;  // mirrors `Odometry odom_;` in xmNabla
  EXPECT_TRUE(o.pose.position.isZero());
  EXPECT_DOUBLE_EQ(o.pose.orientation.w(), 1.0);
  EXPECT_TRUE(o.twist.linear.isZero());
  EXPECT_TRUE(o.child_frame_id.empty());
}

// Existing aggregate-init usage in xmNabla must still compile/behave:
//   odom_.pose  = {{0,0,0}, {1,0,0,0}};
//   odom_.twist = {{0,0,0}, {0,0,0}};
TEST(TypesTest, AggregateInitStillWorks) {
  xmotion::Pose p = {{1, 2, 3}, {1, 0, 0, 0}};
  EXPECT_DOUBLE_EQ(p.position.x(), 1.0);
  EXPECT_DOUBLE_EQ(p.orientation.w(), 1.0);
  xmotion::Twist t = {{0, 0, 0}, {0, 0, 0}};
  EXPECT_TRUE(t.linear.isZero());
}

// --- time vocabulary --------------------------------------------------------

TEST(TypesTest, TimeIsMonotonic) {
  const xmotion::Timestamp a = xmotion::Now();
  const xmotion::Timestamp b = xmotion::Now();
  EXPECT_GE(b, a);
  // Clock alias is monotonic (steady), and the legacy spelling still resolves.
  static_assert(std::is_same<xmotion::RSClock, xmotion::Clock>::value,
                "RSClock must alias Clock for source compatibility");
  static_assert(xmotion::Clock::is_steady, "Clock must be a steady clock");
}

// --- Stamped<T> -------------------------------------------------------------

TEST(TypesTest, StampedPairsValueWithTime) {
  auto s = xmotion::StampNow(42);
  EXPECT_EQ(s.value, 42);
  EXPECT_GE(s.stamp, xmotion::Timestamp{});
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
