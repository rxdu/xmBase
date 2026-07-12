/*
 * test_types_core.cpp — the Eigen-FREE wire tier of the type vocabulary
 * (scalar, time, POD vectors, Stamped<T>, and the base_types facade).
 *
 * This test links ONLY the core xmBase target: it must build and pass with
 * no Eigen on the include path (the 0.5.0 target split's proof at the test
 * tier). Geometry/quantities coverage lives in test_types.cpp, which links
 * xmBaseGeometry.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <type_traits>

#include "gtest/gtest.h"

// Legacy wire-tier facade must keep resolving (xmDriver source compat).
#include "xmbase/types/base_types.hpp"
#include "xmbase/types/scalar.hpp"
#include "xmbase/types/stamped.hpp"
#include "xmbase/types/time.hpp"
#include "xmbase/types/vector.hpp"

namespace {

TEST(TypesCoreTest, PodVectorsDefaultToZero) {
  xmotion::Vector3f v3{};
  EXPECT_FLOAT_EQ(v3.x, 0.0f);
  EXPECT_FLOAT_EQ(v3.y, 0.0f);
  EXPECT_FLOAT_EQ(v3.z, 0.0f);

  xmotion::Vector4d v4{};
  EXPECT_DOUBLE_EQ(v4.w, 0.0);
}

TEST(TypesCoreTest, TimeIsMonotonic) {
  const xmotion::Timestamp a = xmotion::Now();
  const xmotion::Timestamp b = xmotion::Now();
  EXPECT_GE(b, a);
  // Clock alias is monotonic (steady), and the legacy spelling still resolves.
  static_assert(std::is_same<xmotion::RSClock, xmotion::Clock>::value,
                "RSClock must alias Clock for source compatibility");
  static_assert(xmotion::Clock::is_steady, "Clock must be a steady clock");
}

TEST(TypesCoreTest, StampedPairsValueWithTime) {
  auto s = xmotion::StampNow(42);
  EXPECT_EQ(s.value, 42);
  EXPECT_GE(s.stamp, xmotion::Timestamp{});
}

}  // namespace
