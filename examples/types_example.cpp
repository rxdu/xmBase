/*
 * types_example.cpp
 *
 * The common type vocabulary: Eigen-backed geometry, the time vocabulary,
 * Stamped<T>, and the opt-in strong-typed quantities. This is an example, NOT a
 * unit test (no assertions); see ../test/test_types.cpp.
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#include <thread>

#include "xmbase/telemetry/telemetry.hpp"
#include "xmbase/types/types.hpp"        // scalar/time/vector/geometry/stamped
#include "xmbase/types/quantities.hpp"   // opt-in strong quantities

using namespace xmotion;

int main() {
  // --- geometry: composite types default-construct to identity/zero ---------
  constexpr double kHalfPi = 1.57079632679489661923;  // 90 deg, in radians
  Pose pose;  // position = (0,0,0), orientation = identity (NOT garbage)
  pose.position = Position3d(1.0, 2.0, 0.5);
  pose.orientation =
      Quaterniond(Eigen::AngleAxisd(kHalfPi, Eigen::Vector3d::UnitZ()));
  XM_INFO("pose: p=({:.2f},{:.2f},{:.2f}) yaw_w={:.3f}", pose.position.x(),
            pose.position.y(), pose.position.z(), pose.orientation.w());

  Twist cmd;
  cmd.linear = Velocity3d(0.4, 0.0, 0.0);   // m/s, body frame
  cmd.angular = Velocity3d(0.0, 0.0, 0.2);  // rad/s

  Odometry odom;
  odom.frame_id = "odom";
  odom.child_frame_id = "base_link";
  odom.pose = pose;
  odom.twist = cmd;
  odom.stamp = Now();
  XM_INFO("odometry: {} in {} (vx={:.2f} wz={:.2f})", odom.child_frame_id,
            odom.frame_id, odom.twist.linear.x(), odom.twist.angular.z());

  // --- time vocabulary: monotonic, nanosecond resolution --------------------
  const Timestamp t0 = Now();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const Duration elapsed = Now() - t0;
  XM_INFO("elapsed {} us",
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed)
                .count());

  // --- Stamped<T>: pair a value with the time it was produced ---------------
  Stamped<Pose> stamped_pose = StampNow(pose);
  XM_INFO("stamped pose captured at t={} ns",
            stamped_pose.stamp.time_since_epoch().count());

  // --- opt-in strong quantities: distinct types, closed arithmetic ----------
  Force f_grav(0.0, 0.0, -9.81);
  Force f_push(2.0, 0.0, 0.0);
  Force f_net = f_grav + f_push;       // Force + Force -> Force
  Torque tau(0.0, 0.0, 0.5);
  XM_INFO("net force = ({:.2f},{:.2f},{:.2f}), |tau| = {:.2f}", f_net.x(),
            f_net.y(), f_net.z(), tau.norm());

  // The escape hatch reaches Eigen for real math:
  const double work = f_push.vec().dot(pose.position);  // F . d
  XM_INFO("work along displacement = {:.3f} J", work);

  // The whole point of strong typing — these would NOT compile, the type system
  // rejects the mix-up:
  //   Velocity3d v = f_net;          // ok: both are Eigen::Vector3d aliases (!)
  //   LinearVelocity lv = f_net;     // ERROR: Force is not a LinearVelocity
  //   Force bad = f_net + tau;       // ERROR: Force + Torque is not defined

  return 0;
}
